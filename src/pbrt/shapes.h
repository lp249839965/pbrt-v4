// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#ifndef PBRT_SHAPES_H
#define PBRT_SHAPES_H

#include <pbrt/pbrt.h>

#include <pbrt/base/shape.h>
#include <pbrt/interaction.h>
#include <pbrt/ray.h>
#include <pbrt/util/mesh.h>
#include <pbrt/util/pstd.h>
#include <pbrt/util/sampling.h>
#include <pbrt/util/transform.h>
#include <pbrt/util/vecmath.h>

#include <map>
#include <memory>
#include <vector>

namespace pbrt {

// ShapeSample Definition
struct ShapeSample {
    Interaction intr;
    Float pdf;
    std::string ToString() const;
};

// ShapeSampleContext Definition
class ShapeSampleContext {
  public:
    ShapeSampleContext() = default;
    PBRT_CPU_GPU
    ShapeSampleContext(const Point3fi &pi, const Normal3f &n, const Normal3f &ns,
                       Float time)
        : pi(pi), n(n), ns(ns), time(time) {}

    PBRT_CPU_GPU
    ShapeSampleContext(const SurfaceInteraction &si)
        : pi(si.pi), n(si.n), ns(si.shading.n), time(si.time) {}
    PBRT_CPU_GPU
    ShapeSampleContext(const MediumInteraction &mi) : pi(mi.pi), time(mi.time) {}

    PBRT_CPU_GPU
    Point3f p() const { return Point3f(pi); }

    PBRT_CPU_GPU
    Point3f OffsetRayOrigin(const Vector3f &w) const {
        // Copied from Interaction... :-p
        Float d = Dot(Abs(n), pi.Error());
        Vector3f offset = d * Vector3f(n);
        if (Dot(w, n) < 0)
            offset = -offset;
        Point3f po = Point3f(pi) + offset;
        // Round offset point _po_ away from _p_
        for (int i = 0; i < 3; ++i) {
            if (offset[i] > 0)
                po[i] = NextFloatUp(po[i]);
            else if (offset[i] < 0)
                po[i] = NextFloatDown(po[i]);
        }

        return po;
    }

    PBRT_CPU_GPU
    Point3f OffsetRayOrigin(const Point3f &pt) const { return OffsetRayOrigin(pt - p()); }

    PBRT_CPU_GPU
    Ray SpawnRay(const Vector3f &w) const {
        // Note: doesn't set medium, but that's fine, since this is only
        // used by shapes to see if ray would have intersected them
        return Ray(OffsetRayOrigin(w), w, time);
    }

    Point3fi pi;
    Normal3f n, ns;
    Float time;
};

// ShapeIntersection Definition
struct ShapeIntersection {
    SurfaceInteraction intr;
    Float tHit;
    std::string ToString() const;
};

// QuadricIntersection Definition
struct QuadricIntersection {
    Float tHit;
    Point3f pObj;
    Float phi;
};

// Sphere Definition
class Sphere {
  public:
    // Sphere Public Methods
    static Sphere *Create(const Transform *renderFromObject,
                          const Transform *objectFromRender, bool reverseOrientation,
                          const ParameterDictionary &parameters, const FileLoc *loc,
                          Allocator alloc);

    std::string ToString() const;

    Sphere(const Transform *renderFromObject, const Transform *objectFromRender,
           bool reverseOrientation, Float radius, Float zMin, Float zMax, Float phiMax)
        : renderFromObject(renderFromObject),
          objectFromRender(objectFromRender),
          reverseOrientation(reverseOrientation),
          transformSwapsHandedness(renderFromObject->SwapsHandedness()),
          radius(radius),
          zMin(Clamp(std::min(zMin, zMax), -radius, radius)),
          zMax(Clamp(std::max(zMin, zMax), -radius, radius)),
          thetaZMin(std::acos(Clamp(std::min(zMin, zMax) / radius, -1, 1))),
          thetaZMax(std::acos(Clamp(std::max(zMin, zMax) / radius, -1, 1))),
          phiMax(Radians(Clamp(phiMax, 0, 360))) {}

    PBRT_CPU_GPU
    Bounds3f Bounds() const;

    PBRT_CPU_GPU
    DirectionCone NormalBounds() const { return DirectionCone::EntireSphere(); }

    PBRT_CPU_GPU
    pstd::optional<ShapeIntersection> Intersect(const Ray &ray,
                                                Float tMax = Infinity) const {
        pstd::optional<QuadricIntersection> isect = BasicIntersect(ray, tMax);
        if (!isect)
            return {};
        SurfaceInteraction intr = InteractionFromIntersection(*isect, -ray.d, ray.time);
        return {{intr, isect->tHit}};
    }

    PBRT_CPU_GPU
    pstd::optional<QuadricIntersection> BasicIntersect(const Ray &r, Float tMax) const {
        Float phi;
        Point3f pHit;
        // Transform _Ray_ to object space
        Point3fi oi = (*objectFromRender)(Point3fi(r.o));
        Vector3fi di = (*objectFromRender)(Vector3fi(r.d));
        Ray ray(Point3f(oi), Vector3f(di), r.time, r.medium);

        // Solve quadratic to compute sphere _t0_ and _t1_
        FloatInterval t0, t1;
        if (!SphereQuadratic(oi, di, &t0, &t1))
            return {};

        // Check quadric shape _t0_ and _t1_ for nearest intersection
        if (t0.UpperBound() > tMax || t1.LowerBound() <= 0)
            return {};
        FloatInterval tShapeHit = t0;
        if (tShapeHit.LowerBound() <= 0) {
            tShapeHit = t1;
            if (tShapeHit.UpperBound() > tMax)
                return {};
        }

        // Compute sphere hit position and $\phi$
        pHit = ray((Float)tShapeHit);
        // Refine sphere intersection point
        pHit *= radius / Distance(pHit, Point3f(0, 0, 0));

        if (pHit.x == 0 && pHit.y == 0)
            pHit.x = 1e-5f * radius;
        phi = std::atan2(pHit.y, pHit.x);
        if (phi < 0)
            phi += 2 * Pi;

        // Test sphere intersection against clipping parameters
        if ((zMin > -radius && pHit.z < zMin) || (zMax < radius && pHit.z > zMax) ||
            phi > phiMax) {
            if (tShapeHit == t1)
                return {};
            if (t1.UpperBound() > tMax)
                return {};
            tShapeHit = t1;
            // Compute sphere hit position and $\phi$
            pHit = ray((Float)tShapeHit);
            // Refine sphere intersection point
            pHit *= radius / Distance(pHit, Point3f(0, 0, 0));

            if (pHit.x == 0 && pHit.y == 0)
                pHit.x = 1e-5f * radius;
            phi = std::atan2(pHit.y, pHit.x);
            if (phi < 0)
                phi += 2 * Pi;

            if ((zMin > -radius && pHit.z < zMin) || (zMax < radius && pHit.z > zMax) ||
                phi > phiMax)
                return {};
        }

        // Return _QuadricIntersection_ for sphere intersection
        return QuadricIntersection{Float(tShapeHit), pHit, phi};
    }

    PBRT_CPU_GPU
    SurfaceInteraction InteractionFromIntersection(const QuadricIntersection &isect,
                                                   const Vector3f &wo, Float time) const {
        Point3f pHit = isect.pObj;
        Float phi = isect.phi;
        // Find parametric representation of sphere hit
        Float u = phi / phiMax;
        Float cosTheta = pHit.z / radius;
        Float theta = SafeACos(cosTheta);
        Float v = (theta - thetaZMin) / (thetaZMax - thetaZMin);
        // Compute sphere $\dpdu$ and $\dpdv$
        Float zRadius = std::sqrt(pHit.x * pHit.x + pHit.y * pHit.y);
        Float invZRadius = 1 / zRadius;
        Float cosPhi = pHit.x * invZRadius;
        Float sinPhi = pHit.y * invZRadius;
        Vector3f dpdu(-phiMax * pHit.y, phiMax * pHit.x, 0);
        Float sinTheta = SafeSqrt(1 - cosTheta * cosTheta);
        Vector3f dpdv = (thetaZMax - thetaZMin) *
                        Vector3f(pHit.z * cosPhi, pHit.z * sinPhi, -radius * sinTheta);

        // Compute sphere $\dndu$ and $\dndv$
        Vector3f d2Pduu = -phiMax * phiMax * Vector3f(pHit.x, pHit.y, 0);
        Vector3f d2Pduv =
            (thetaZMax - thetaZMin) * pHit.z * phiMax * Vector3f(-sinPhi, cosPhi, 0.);
        Vector3f d2Pdvv = -(thetaZMax - thetaZMin) * (thetaZMax - thetaZMin) *
                          Vector3f(pHit.x, pHit.y, pHit.z);
        // Compute coefficients for fundamental forms
        Float E = Dot(dpdu, dpdu);
        Float F = Dot(dpdu, dpdv);
        Float G = Dot(dpdv, dpdv);
        Vector3f N = Normalize(Cross(dpdu, dpdv));
        Float e = Dot(N, d2Pduu);
        Float f = Dot(N, d2Pduv);
        Float g = Dot(N, d2Pdvv);

        // Compute $\dndu$ and $\dndv$ from fundamental form coefficients
        Float invEGF2 = 1 / (E * G - F * F);
        Normal3f dndu =
            Normal3f((f * F - e * G) * invEGF2 * dpdu + (e * F - f * E) * invEGF2 * dpdv);
        Normal3f dndv =
            Normal3f((g * F - f * G) * invEGF2 * dpdu + (f * F - g * E) * invEGF2 * dpdv);

        // Compute error bounds for sphere intersection
        Vector3f pError = gamma(5) * Abs((Vector3f)pHit);

        // Return _SurfaceInteraction_ for quadric intersection
        return (*renderFromObject)(SurfaceInteraction(
            Point3fi(pHit, pError), Point2f(u, v), (*objectFromRender)(wo), dpdu, dpdv,
            dndu, dndv, time, reverseOrientation ^ transformSwapsHandedness));
    }

    PBRT_CPU_GPU
    bool IntersectP(const Ray &r, Float tMax = Infinity) const {
        return BasicIntersect(r, tMax).has_value();
    }

    PBRT_CPU_GPU
    Float Area() const { return phiMax * radius * (zMax - zMin); }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const Point2f &u) const;

    PBRT_CPU_GPU
    Float PDF(const Interaction &) const { return 1 / Area(); }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const ShapeSampleContext &ctx,
                                       const Point2f &u) const {
        Point3f pCenter = (*renderFromObject)(Point3f(0, 0, 0));
        // Sample uniformly on sphere if $\pt{}$ is inside it
        Point3f pOrigin = ctx.OffsetRayOrigin(pCenter);
        if (DistanceSquared(pOrigin, pCenter) <= radius * radius) {
            pstd::optional<ShapeSample> ss = Sample(u);
            if (!ss)
                return {};
            Vector3f wi = ss->intr.p() - ctx.p();
            if (LengthSquared(wi) == 0)
                return {};
            else {
                // Convert from area measure returned by Sample() call above to
                // solid angle measure.
                wi = Normalize(wi);
                ss->pdf *=
                    DistanceSquared(ctx.p(), ss->intr.p()) / AbsDot(ss->intr.n, -wi);
            }
            if (std::isinf(ss->pdf))
                return {};
            return ss;
        }

        // Compute coordinate system for sphere sampling
        Frame samplingFrame = Frame::FromZ(Normalize(ctx.p() - pCenter));

        // Sample sphere uniformly inside subtended cone
        // Compute $\theta$ and $\phi$ values for sample in cone
        Float dc = Distance(ctx.p(), pCenter);
        Float invDc = 1 / dc;
        Float sinThetaMax = radius * invDc;
        Float sinThetaMax2 = sinThetaMax * sinThetaMax;
        Float invSinThetaMax = 1 / sinThetaMax;
        Float cosThetaMax = SafeSqrt(1 - sinThetaMax2);
        Float oneMinusCosThetaMax = 1 - cosThetaMax;
        Float cosTheta = (cosThetaMax - 1) * u[0] + 1;
        Float sinTheta2 = 1 - cosTheta * cosTheta;

        if (sinThetaMax2 < 0.00068523f /* sin^2(1.5 deg) */) {
            /* Fall back to a Taylor series expansion for small angles, where
               the standard approach suffers from severe cancellation errors */
            sinTheta2 = sinThetaMax2 * u[0];
            cosTheta = std::sqrt(1 - sinTheta2);

            // Taylor expansion of 1 - sqrt(1 - Sqr(sinThetaMax)) at 0...
            oneMinusCosThetaMax = sinThetaMax2 / 2;
        }

        // Compute angle $\alpha$ from center of sphere to sampled point on surface
        Float cosAlpha =
            sinTheta2 * invSinThetaMax +
            cosTheta * SafeSqrt(1 - sinTheta2 * invSinThetaMax * invSinThetaMax);
        Float sinAlpha = SafeSqrt(1 - cosAlpha * cosAlpha);

        // Compute surface normal and sampled point on sphere
        Float phi = u[1] * 2 * Pi;
        Vector3f nRender =
            samplingFrame.FromLocal(SphericalDirection(sinAlpha, cosAlpha, phi));
        Point3f pRender = pCenter + radius * Point3f(nRender.x, nRender.y, nRender.z);
        Vector3f pError = gamma(5) * Abs((Vector3f)pRender);
        Point3fi pi(pRender, pError);
        Normal3f n(nRender);
        if (reverseOrientation)
            n *= -1;

        // Return _ShapeSample_ for sampled point on sphere
        DCHECK_NE(oneMinusCosThetaMax, 0);  // very small far away sphere
        return ShapeSample{Interaction(pi, n, ctx.time),
                           1 / (2 * Pi * oneMinusCosThetaMax)};
    }

    PBRT_CPU_GPU
    Float PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const {
        Point3f pCenter = (*renderFromObject)(Point3f(0, 0, 0));
        // Return uniform PDF if point is inside sphere
        Point3f pOrigin = ctx.OffsetRayOrigin(pCenter);
        if (DistanceSquared(pOrigin, pCenter) <= radius * radius) {
            pstd::optional<ShapeIntersection> isect = Intersect(Ray(pOrigin, wi));
            CHECK_RARE(1e-6, !isect.has_value());
            if (!isect)
                return 0;
            Float pdf = DistanceSquared(pOrigin, isect->intr.p()) /
                        (AbsDot(isect->intr.n, -wi) * Area());
            if (std::isinf(pdf))
                pdf = 0.f;
            return pdf;
        }

        // Compute general sphere PDF
        Float sinThetaMax2 = radius * radius / DistanceSquared(ctx.p(), pCenter);
        Float cosThetaMax = SafeSqrt(1 - sinThetaMax2);
        Float oneMinusCosThetaMax = 1 - cosThetaMax;

        if (sinThetaMax2 < 0.00068523f /* sin^2(1.5 deg) */)
            oneMinusCosThetaMax = sinThetaMax2 / 2;

        return 1 / (2 * Pi * oneMinusCosThetaMax);
    }

  private:
    // Sphere Private Methods
    PBRT_CPU_GPU
    bool SphereQuadratic(const Point3fi &o, const Vector3fi &d, FloatInterval *t0,
                         FloatInterval *t1) const {
        /* Recap of the approach from Ray Tracing Gems:

           The basic idea is to rewrite b^2 - 4ac to 4a (b^2/4a - c),
           then simplify that to 4a * (r^2 - (Dot(o, o) - Dot(o,
           d)^2/LengthSquared(d)) = 4a (r^2 - (Dot(o, o) - Dot(o, d^))) where d^
           is Normalize(d). Now, consider the decomposition of o into the sum of
           two vectors, d_perp and d_parl, where d_parl is parallel to d^. We
           have d_parl = Dot(o, d^) d^, and d_perp = o - d_parl = o - Dot(o, d^)
           d^. We have a right triangle formed by o, d_perp, and d_parl, and so
           |o|^2 = |d_perp|^2 + |d_parl|^2.
           Note that |d_parl|^2 = Dot(o, d^)^2. Subtrace |d_parl|^2 from both
           sides and we have Dot(o, o) - Dot(o, d^)^2 = |o - Dot(o, d^) d^|^2.

           With the conventional approach, when the ray is long, we end up with
           b^2 \approx 4ac and get hit with catastrophic cancellation.  It's
           extra bad since the magnitudes of the two terms are related to the
           *squared* distance to the ray origin. Even for rays that massively
           miss, we'll end up with a discriminant exactly equal to zero, and
           thence a reported intersection.

           The new approach eliminates c from the computation of the
           discriminant: that's a big win, since it's magnitude is proportional
           to the squared distance to the origin, with accordingly limited
           precision ("accuracy"?)

           Note: the error in the old one is best visualized by going to the
           checkout *before* 1d6e7bd9f6e10991d0c75a2ec74026a2a453522c
           (otherwise everything disappears, since there's too much error in
           the discriminant.)
        */
        // Initialize _FloatInterval_ ray coordinate values
        FloatInterval a = SumSquares(d.x, d.y, d.z);
        FloatInterval b = 2 * (d.x * o.x + d.y * o.y + d.z * o.z);
        FloatInterval c = SumSquares(o.x, o.y, o.z) - Sqr(FloatInterval(radius));

        // Solve quadratic equation for _t_ values
#if 0
    // Original
    FloatInterval b2 = Sqr(b), ac = 4 * a * c;
    FloatInterval odiscrim = b2 - ac; // b * b - FloatInterval(4) * a * c;
#endif
        // RT Gems
        FloatInterval f = b / (2 * a);  // (o . d) / LengthSquared(d)
        Point3fi fp = o - f * d;
        // There's a bit more precision if you compute x^2-y^2 as (x+y)(x-y).
        FloatInterval sqrtf = Sqrt(SumSquares(fp.x, fp.y, fp.z));
        FloatInterval discrim =
            4 * a * ((FloatInterval(radius)) - sqrtf) * ((FloatInterval(radius)) + sqrtf);

        if (discrim.LowerBound() < 0)
            return {};
        FloatInterval rootDiscrim = Sqrt(discrim);

        // Compute quadratic _t_ values
        FloatInterval q;
        if ((Float)b < 0)
            q = -.5 * (b - rootDiscrim);
        else
            q = -.5 * (b + rootDiscrim);
        *t0 = q / a;
        *t1 = c / q;
        if (t0->LowerBound() > t1->LowerBound())
            pstd::swap(*t0, *t1);
        return true;
    }

    // Sphere Private Members
    Float radius;
    Float zMin, zMax;
    Float thetaZMin, thetaZMax, phiMax;
    const Transform *renderFromObject, *objectFromRender;
    bool reverseOrientation;
    bool transformSwapsHandedness;
};

// Disk Definition
class Disk {
  public:
    // Disk Public Methods
    Disk(const Transform *renderFromObject, const Transform *objectFromRender,
         bool reverseOrientation, Float height, Float radius, Float innerRadius,
         Float phiMax)
        : renderFromObject(renderFromObject),
          objectFromRender(objectFromRender),
          reverseOrientation(reverseOrientation),
          transformSwapsHandedness(renderFromObject->SwapsHandedness()),
          height(height),
          radius(radius),
          innerRadius(innerRadius),
          phiMax(Radians(Clamp(phiMax, 0, 360))) {}

    static Disk *Create(const Transform *renderFromObject,
                        const Transform *objectFromRender, bool reverseOrientation,
                        const ParameterDictionary &parameters, const FileLoc *loc,
                        Allocator alloc);

    std::string ToString() const;

    PBRT_CPU_GPU
    Float Area() const {
        return phiMax * 0.5f * (radius * radius - innerRadius * innerRadius);
    }

    PBRT_CPU_GPU
    Bounds3f Bounds() const;

    PBRT_CPU_GPU
    DirectionCone NormalBounds() const;

    PBRT_CPU_GPU
    pstd::optional<ShapeIntersection> Intersect(const Ray &ray,
                                                Float tMax = Infinity) const {
        pstd::optional<QuadricIntersection> isect = BasicIntersect(ray, tMax);
        if (!isect)
            return {};
        SurfaceInteraction intr = InteractionFromIntersection(*isect, -ray.d, ray.time);
        return {{intr, isect->tHit}};
    }

    PBRT_CPU_GPU
    pstd::optional<QuadricIntersection> BasicIntersect(const Ray &r, Float tMax) const {
        // Transform _Ray_ to object space
        Point3fi oi = (*objectFromRender)(Point3fi(r.o));
        Vector3fi di = (*objectFromRender)(Vector3fi(r.d));
        Ray ray(Point3f(oi), Vector3f(di), r.time, r.medium);

        // Compute plane intersection for disk
        // Reject disk intersections for rays parallel to the disk's plane
        if (ray.d.z == 0)
            return {};

        Float tShapeHit = (height - ray.o.z) / ray.d.z;
        if (tShapeHit <= 0 || tShapeHit >= tMax)
            return {};

        // See if hit point is inside disk radii and $\phimax$
        Point3f pHit = ray(tShapeHit);
        Float dist2 = pHit.x * pHit.x + pHit.y * pHit.y;
        if (dist2 > radius * radius || dist2 < innerRadius * innerRadius)
            return {};
        // Test disk $\phi$ value against $\phimax$
        Float phi = std::atan2(pHit.y, pHit.x);
        if (phi < 0)
            phi += 2 * Pi;
        if (phi > phiMax)
            return {};

        // Return _QuadricIntersection_ for disk intersection
        return QuadricIntersection{tShapeHit, pHit, phi};
    }

    PBRT_CPU_GPU
    SurfaceInteraction InteractionFromIntersection(const QuadricIntersection &isect,
                                                   const Vector3f &wo, Float time) const {
        Point3f pHit = isect.pObj;
        Float phi = isect.phi;
        Float dist2 = pHit.x * pHit.x + pHit.y * pHit.y;
        // Find parametric representation of disk hit
        Float u = phi / phiMax;
        Float rHit = std::sqrt(dist2);
        Float v = (radius - rHit) / (radius - innerRadius);
        Vector3f dpdu(-phiMax * pHit.y, phiMax * pHit.x, 0);
        Vector3f dpdv = Vector3f(pHit.x, pHit.y, 0.) * (innerRadius - radius) / rHit;
        Normal3f dndu(0, 0, 0), dndv(0, 0, 0);

        // Refine disk intersection point
        pHit.z = height;

        // Compute error bounds for disk intersection
        Vector3f pError(0, 0, 0);

        // Return _SurfaceInteraction_ for quadric intersection
        return (*renderFromObject)(SurfaceInteraction(
            Point3fi(pHit, pError), Point2f(u, v), (*objectFromRender)(wo), dpdu, dpdv,
            dndu, dndv, time, reverseOrientation ^ transformSwapsHandedness));
    }

    PBRT_CPU_GPU
    bool IntersectP(const Ray &r, Float tMax = Infinity) const {
        return BasicIntersect(r, tMax).has_value();
    }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const Point2f &u) const {
        Point2f pd = SampleUniformDiskConcentric(u);
        Point3f pObj(pd.x * radius, pd.y * radius, height);
        Point3fi pi = (*renderFromObject)(Point3fi(pObj));
        Normal3f n = Normalize((*renderFromObject)(Normal3f(0, 0, 1)));
        if (reverseOrientation)
            n *= -1;
        return ShapeSample{Interaction(pi, n), 1 / Area()};
    }

    PBRT_CPU_GPU
    Float PDF(const Interaction &) const { return 1 / Area(); }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const ShapeSampleContext &ctx,
                                       const Point2f &u) const {
        pstd::optional<ShapeSample> ss = Sample(u);
        if (!ss)
            return ss;

        ss->intr.time = ctx.time;
        Vector3f wi = ss->intr.p() - ctx.p();
        if (LengthSquared(wi) == 0)
            return {};
        else {
            wi = Normalize(wi);
            // Convert from area measure, as returned by the Sample() call
            // above, to solid angle measure.
            ss->pdf *= DistanceSquared(ctx.p(), ss->intr.p()) / AbsDot(ss->intr.n, -wi);
            if (std::isinf(ss->pdf))
                return {};
        }
        return ss;
    }

    PBRT_CPU_GPU
    Float PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const {
        // Intersect sample ray with area light geometry
        Ray ray = ctx.SpawnRay(wi);
        pstd::optional<ShapeIntersection> si = Intersect(ray);
        if (!si)
            return 0;

        // Convert light sample weight to solid angle measure
        Float pdf =
            DistanceSquared(ctx.p(), si->intr.p()) / (AbsDot(si->intr.n, -wi) * Area());
        if (std::isinf(pdf))
            pdf = 0.f;
        return pdf;
    }

  private:
    // Disk Private Members
    const Transform *renderFromObject, *objectFromRender;
    bool reverseOrientation;
    bool transformSwapsHandedness;
    Float height, radius, innerRadius, phiMax;
};

// Cylinder Definition
class Cylinder {
  public:
    // Cylinder Public Methods
    Cylinder(const Transform *renderFromObject, const Transform *objectFromRender,
             bool reverseOrientation, Float radius, Float zMin, Float zMax, Float phiMax)
        : renderFromObject(renderFromObject),
          objectFromRender(objectFromRender),
          reverseOrientation(reverseOrientation),
          transformSwapsHandedness(renderFromObject->SwapsHandedness()),
          radius(radius),
          zMin(std::min(zMin, zMax)),
          zMax(std::max(zMin, zMax)),
          phiMax(Radians(Clamp(phiMax, 0, 360))) {}

    static Cylinder *Create(const Transform *renderFromObject,
                            const Transform *objectFromRender, bool reverseOrientation,
                            const ParameterDictionary &parameters, const FileLoc *loc,
                            Allocator alloc);

    PBRT_CPU_GPU
    Bounds3f Bounds() const;

    std::string ToString() const;

    PBRT_CPU_GPU
    Float Area() const { return (zMax - zMin) * radius * phiMax; }

    PBRT_CPU_GPU
    DirectionCone NormalBounds() const { return DirectionCone::EntireSphere(); }

    PBRT_CPU_GPU
    pstd::optional<ShapeIntersection> Intersect(const Ray &ray,
                                                Float tMax = Infinity) const {
        pstd::optional<QuadricIntersection> isect = BasicIntersect(ray, tMax);
        if (!isect)
            return {};

        SurfaceInteraction intr = InteractionFromIntersection(*isect, -ray.d, ray.time);
        return {{intr, isect->tHit}};
    }

    PBRT_CPU_GPU
    pstd::optional<QuadricIntersection> BasicIntersect(const Ray &r, Float tMax) const {
        Float phi;
        Point3f pHit;
        // Transform _Ray_ to object space
        Point3fi oi = (*objectFromRender)(Point3fi(r.o));
        Vector3fi di = (*objectFromRender)(Vector3fi(r.d));
        Ray ray(Point3f(oi), Vector3f(di), r.time, r.medium);

        // Compute quadratic cylinder coefficients
        FloatInterval t0, t1;
        if (!CylinderQuadratic(oi, di, &t0, &t1))
            return {};

        // Check quadric shape _t0_ and _t1_ for nearest intersection
        if (t0.UpperBound() > tMax || t1.LowerBound() <= 0)
            return {};
        FloatInterval tShapeHit = t0;
        if (tShapeHit.LowerBound() <= 0) {
            tShapeHit = t1;
            if (tShapeHit.UpperBound() > tMax)
                return {};
        }

        // Compute cylinder hit point and $\phi$
        pHit = ray((Float)tShapeHit);
        // Refine cylinder intersection point
        Float hitRad = std::sqrt(pHit.x * pHit.x + pHit.y * pHit.y);
        pHit.x *= radius / hitRad;
        pHit.y *= radius / hitRad;

        phi = std::atan2(pHit.y, pHit.x);
        if (phi < 0)
            phi += 2 * Pi;

        // Test cylinder intersection against clipping parameters
        if (pHit.z < zMin || pHit.z > zMax || phi > phiMax) {
            if (tShapeHit == t1)
                return {};
            tShapeHit = t1;
            if (t1.UpperBound() > tMax)
                return {};
            // Compute cylinder hit point and $\phi$
            pHit = ray((Float)tShapeHit);
            // Refine cylinder intersection point
            Float hitRad = std::sqrt(pHit.x * pHit.x + pHit.y * pHit.y);
            pHit.x *= radius / hitRad;
            pHit.y *= radius / hitRad;

            phi = std::atan2(pHit.y, pHit.x);
            if (phi < 0)
                phi += 2 * Pi;

            if (pHit.z < zMin || pHit.z > zMax || phi > phiMax)
                return {};
        }

        // Return _QuadricIntersection_ for cylinder intersection
        return QuadricIntersection{(Float)tShapeHit, pHit, phi};
    }

    PBRT_CPU_GPU
    SurfaceInteraction InteractionFromIntersection(const QuadricIntersection &isect,
                                                   const Vector3f &wo, Float time) const {
        Point3f pHit = isect.pObj;
        Float phi = isect.phi;
        // Find parametric representation of cylinder hit
        Float u = phi / phiMax;
        Float v = (pHit.z - zMin) / (zMax - zMin);
        // Compute cylinder $\dpdu$ and $\dpdv$
        Vector3f dpdu(-phiMax * pHit.y, phiMax * pHit.x, 0);
        Vector3f dpdv(0, 0, zMax - zMin);

        // Compute cylinder $\dndu$ and $\dndv$
        Vector3f d2Pduu = -phiMax * phiMax * Vector3f(pHit.x, pHit.y, 0);
        Vector3f d2Pduv(0, 0, 0), d2Pdvv(0, 0, 0);
        // Compute coefficients for fundamental forms
        Float E = Dot(dpdu, dpdu);
        Float F = Dot(dpdu, dpdv);
        Float G = Dot(dpdv, dpdv);
        Vector3f N = Normalize(Cross(dpdu, dpdv));
        Float e = Dot(N, d2Pduu);
        Float f = Dot(N, d2Pduv);
        Float g = Dot(N, d2Pdvv);

        // Compute $\dndu$ and $\dndv$ from fundamental form coefficients
        Float invEGF2 = 1 / (E * G - F * F);
        Normal3f dndu =
            Normal3f((f * F - e * G) * invEGF2 * dpdu + (e * F - f * E) * invEGF2 * dpdv);
        Normal3f dndv =
            Normal3f((g * F - f * G) * invEGF2 * dpdu + (f * F - g * E) * invEGF2 * dpdv);

        // Compute error bounds for cylinder intersection
        Vector3f pError = gamma(3) * Abs(Vector3f(pHit.x, pHit.y, 0));

        // Return _SurfaceInteraction_ for quadric intersection
        return (*renderFromObject)(SurfaceInteraction(
            Point3fi(pHit, pError), Point2f(u, v), (*objectFromRender)(wo), dpdu, dpdv,
            dndu, dndv, time, reverseOrientation ^ transformSwapsHandedness));
    }

    PBRT_CPU_GPU
    bool IntersectP(const Ray &r, Float tMax = Infinity) const {
        return BasicIntersect(r, tMax).has_value();
    }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const Point2f &u) const {
        Float z = Lerp(u[0], zMin, zMax);
        Float phi = u[1] * phiMax;
        Point3f pObj = Point3f(radius * std::cos(phi), radius * std::sin(phi), z);
        // Reproject _pObj_ to cylinder surface and compute _pObjError_
        Float hitRad = std::sqrt(pObj.x * pObj.x + pObj.y * pObj.y);
        pObj.x *= radius / hitRad;
        pObj.y *= radius / hitRad;
        Vector3f pObjError = gamma(3) * Abs(Vector3f(pObj.x, pObj.y, 0));

        Point3fi pi = (*renderFromObject)(Point3fi(pObj, pObjError));
        Normal3f n = Normalize((*renderFromObject)(Normal3f(pObj.x, pObj.y, 0)));
        if (reverseOrientation)
            n *= -1;
        return ShapeSample{Interaction(pi, n), 1 / Area()};
    }

    PBRT_CPU_GPU
    Float PDF(const Interaction &) const { return 1 / Area(); }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const ShapeSampleContext &ctx,
                                       const Point2f &u) const {
        pstd::optional<ShapeSample> ss = Sample(u);
        if (!ss)
            return ss;

        ss->intr.time = ctx.time;
        Vector3f wi = ss->intr.p() - ctx.p();
        if (LengthSquared(wi) == 0)
            return {};
        else {
            wi = Normalize(wi);
            // Convert from area measure, as returned by the Sample() call
            // above, to solid angle measure.
            ss->pdf *= DistanceSquared(ctx.p(), ss->intr.p()) / AbsDot(ss->intr.n, -wi);
            if (std::isinf(ss->pdf))
                return {};
        }
        return ss;
    }

    PBRT_CPU_GPU
    Float PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const {
        // Intersect sample ray with area light geometry
        Ray ray = ctx.SpawnRay(wi);
        pstd::optional<ShapeIntersection> si = Intersect(ray);
        if (!si)
            return 0;

        // Convert light sample weight to solid angle measure
        Float pdf =
            DistanceSquared(ctx.p(), si->intr.p()) / (AbsDot(si->intr.n, -wi) * Area());
        if (std::isinf(pdf))
            pdf = 0.f;
        return pdf;
    }

  private:
    // Cylinder Private Methods
    PBRT_CPU_GPU
    bool CylinderQuadratic(const Point3fi &oi, const Vector3fi &di, FloatInterval *t0,
                           FloatInterval *t1) const {
        FloatInterval a = SumSquares(di.x, di.y);
        FloatInterval b = 2 * (di.x * oi.x + di.y * oi.y);
        FloatInterval c = SumSquares(oi.x, oi.y) - Sqr(FloatInterval(radius));

        // Solve quadratic equation for _t_ values
        // FloatInterval discrim = B * B - FloatInterval(4) * A * C;
        FloatInterval f = b / (2 * a);  // (o . d) / LengthSquared(d)
        FloatInterval fx = oi.x - f * di.x;
        FloatInterval fy = oi.y - f * di.y;
        FloatInterval sqrtf = Sqrt(SumSquares(fx, fy));
        FloatInterval discrim =
            4 * a * (FloatInterval(radius) + sqrtf) * (FloatInterval(radius) - sqrtf);
        if (discrim.LowerBound() < 0)
            return false;
        FloatInterval rootDiscrim = Sqrt(discrim);

        // Compute quadratic _t_ values
        FloatInterval q;
        if ((Float)b < 0)
            q = -.5 * (b - rootDiscrim);
        else
            q = -.5 * (b + rootDiscrim);
        *t0 = q / a;
        *t1 = c / q;
        if (t0->LowerBound() > t1->LowerBound())
            pstd::swap(*t0, *t1);
        return true;
    }

    // Cylinder Private Members
    const Transform *renderFromObject, *objectFromRender;
    bool reverseOrientation, transformSwapsHandedness;
    Float radius, zMin, zMax, phiMax;
};

// Triangle Declarations
#if defined(PBRT_BUILD_GPU_RENDERER) && defined(__CUDACC__)
extern PBRT_GPU pstd::vector<const TriangleMesh *> *allTriangleMeshesGPU;
#endif

// TriangleIntersection Definition
struct TriangleIntersection {
    Float b0, b1, b2;
    Float t;
    std::string ToString() const;
};

// Triangle Definition
class Triangle {
  public:
    // Triangle Public Methods
    static pstd::vector<ShapeHandle> CreateTriangles(const TriangleMesh *mesh,
                                                     Allocator alloc);

    Triangle() = default;
    Triangle(int meshIndex, int triIndex) : meshIndex(meshIndex), triIndex(triIndex) {}

    static void Init(Allocator alloc);

    PBRT_CPU_GPU
    Bounds3f Bounds() const;

    PBRT_CPU_GPU
    pstd::optional<ShapeIntersection> Intersect(const Ray &ray,
                                                Float tMax = Infinity) const;
    PBRT_CPU_GPU
    bool IntersectP(const Ray &ray, Float tMax = Infinity) const;

    PBRT_CPU_GPU
    bool OrientationIsReversed() const { return GetMesh()->reverseOrientation; }
    PBRT_CPU_GPU
    bool TransformSwapsHandedness() const { return GetMesh()->transformSwapsHandedness; }

    PBRT_CPU_GPU
    Float Area() const {
        // Get triangle vertices in _p0_, _p1_, and _p2_
        auto mesh = GetMesh();
        const int *v = &mesh->vertexIndices[3 * triIndex];
        const Point3f &p0 = mesh->p[v[0]], &p1 = mesh->p[v[1]];
        const Point3f &p2 = mesh->p[v[2]];

        return 0.5f * Length(Cross(p1 - p0, p2 - p0));
    }

    PBRT_CPU_GPU
    DirectionCone NormalBounds() const;

    std::string ToString() const;

    static TriangleMesh *CreateMesh(const Transform *renderFromObject,
                                    bool reverseOrientation,
                                    const ParameterDictionary &parameters,
                                    const FileLoc *loc, Allocator alloc);

    PBRT_CPU_GPU
    static pstd::optional<TriangleIntersection> Intersect(const Ray &ray, Float tMax,
                                                          const Point3f &p0,
                                                          const Point3f &p1,
                                                          const Point3f &p2);

    PBRT_CPU_GPU
    static pstd::optional<SurfaceInteraction> InteractionFromIntersection(
        const TriangleMesh *mesh, int triIndex, pstd::array<Float, 3> b, Float time,
        const Vector3f &wo, pstd::optional<Transform> renderFromInstance = {}) {
        const int *v = &mesh->vertexIndices[3 * triIndex];
        Point3f p0 = mesh->p[v[0]], p1 = mesh->p[v[1]], p2 = mesh->p[v[2]];
        if (renderFromInstance) {
            p0 = (*renderFromInstance)(p0);
            p1 = (*renderFromInstance)(p1);
            p2 = (*renderFromInstance)(p2);
        }
        // Compute triangle partial derivatives
        Vector3f dpdu, dpdv;
        pstd::array<Point2f, 3> triuv =
            mesh->uv
                ? pstd::array<Point2f, 3>(
                      {mesh->uv[v[0]], mesh->uv[v[1]], mesh->uv[v[2]]})
                : pstd::array<Point2f, 3>({Point2f(0, 0), Point2f(1, 0), Point2f(1, 1)});
        // Compute deltas for triangle partial derivatives
        Vector2f duv02 = triuv[0] - triuv[2], duv12 = triuv[1] - triuv[2];
        Vector3f dp02 = p0 - p2, dp12 = p1 - p2;

        Float determinant = DifferenceOfProducts(duv02[0], duv12[1], duv02[1], duv12[0]);
        bool degenerateUV = std::abs(determinant) < 1e-12;
        if (!degenerateUV) {
            Float invdet = 1 / determinant;
            dpdu = DifferenceOfProducts(duv12[1], dp02, duv02[1], dp12) * invdet;
            dpdv = DifferenceOfProducts(duv02[0], dp12, duv12[0], dp02) * invdet;
        }
        if (degenerateUV || LengthSquared(Cross(dpdu, dpdv)) == 0) {
            Vector3f ng = Cross(p2 - p0, p1 - p0);
            if (LengthSquared(ng) == 0) {
                // TODO: should these be eliminated from the start?
                return {};
            }
            // Handle zero determinant for triangle partial derivative matrix
            CoordinateSystem(Normalize(ng), &dpdu, &dpdv);
        }

        // Interpolate $(u,v)$ parametric coordinates and hit point
        Point3f pHit = b[0] * p0 + b[1] * p1 + b[2] * p2;
        Point2f uvHit = b[0] * triuv[0] + b[1] * triuv[1] + b[2] * triuv[2];

        // Compute error bounds for triangle intersection
        Float xAbsSum =
            (std::abs(b[0] * p0.x) + std::abs(b[1] * p1.x) + std::abs(b[2] * p2.x));
        Float yAbsSum =
            (std::abs(b[0] * p0.y) + std::abs(b[1] * p1.y) + std::abs(b[2] * p2.y));
        Float zAbsSum =
            (std::abs(b[0] * p0.z) + std::abs(b[1] * p1.z) + std::abs(b[2] * p2.z));
        Vector3f pError = gamma(7) * Vector3f(xAbsSum, yAbsSum, zAbsSum);

        // Return _SurfaceInteraction_ for triangle hit
        Point3fi pHitError(pHit, pError);
        int faceIndex = mesh->faceIndices != nullptr ? mesh->faceIndices[triIndex] : 0;
        SurfaceInteraction isect(
            pHitError, uvHit, wo, dpdu, dpdv, Normal3f(0, 0, 0), Normal3f(0, 0, 0), time,
            mesh->reverseOrientation ^ mesh->transformSwapsHandedness, faceIndex);
        // Override surface normal in _isect_ for triangle
        isect.n = isect.shading.n = Normal3f(Normalize(Cross(dp02, dp12)));
        if (mesh->reverseOrientation ^ mesh->transformSwapsHandedness)
            isect.n = isect.shading.n = -isect.n;

        if (mesh->n || mesh->s) {
            // Initialize _Triangle_ shading geometry
            // Compute shading normal _ns_ for triangle
            Normal3f ns;
            if (mesh->n != nullptr) {
                ns = (b[0] * mesh->n[v[0]] + b[1] * mesh->n[v[1]] + b[2] * mesh->n[v[2]]);
                if (renderFromInstance)
                    ns = (*renderFromInstance)(ns);

                if (LengthSquared(ns) > 0)
                    ns = Normalize(ns);
                else
                    ns = isect.n;
            } else
                ns = isect.n;

            // Compute shading tangent _ss_ for triangle
            Vector3f ss;
            if (mesh->s != nullptr) {
                ss = (b[0] * mesh->s[v[0]] + b[1] * mesh->s[v[1]] + b[2] * mesh->s[v[2]]);
                if (renderFromInstance)
                    ss = (*renderFromInstance)(ss);

                if (LengthSquared(ss) == 0)
                    ss = isect.dpdu;
            } else
                ss = isect.dpdu;

            // Compute shading bitangent _ts_ for triangle and adjust _ss_
            Vector3f ts = Cross(ns, ss);
            if (LengthSquared(ts) > 0)
                ss = Cross(ts, ns);
            else
                CoordinateSystem(ns, &ss, &ts);

            // Compute $\dndu$ and $\dndv$ for triangle shading geometry
            Normal3f dndu, dndv;
            if (mesh->n != nullptr) {
                // Compute deltas for triangle partial derivatives of normal
                Vector2f duv02 = triuv[0] - triuv[2];
                Vector2f duv12 = triuv[1] - triuv[2];
                Normal3f dn1 = mesh->n[v[0]] - mesh->n[v[2]];
                Normal3f dn2 = mesh->n[v[1]] - mesh->n[v[2]];
                if (renderFromInstance) {
                    dn1 = (*renderFromInstance)(dn1);
                    dn2 = (*renderFromInstance)(dn2);
                }

                Float determinant =
                    DifferenceOfProducts(duv02[0], duv12[1], duv02[1], duv12[0]);
                bool degenerateUV = std::abs(determinant) < 1e-32;
                if (degenerateUV) {
                    // We can still compute dndu and dndv, with respect to the
                    // same arbitrary coordinate system we use to compute dpdu
                    // and dpdv when this happens. It's important to do this
                    // (rather than giving up) so that ray differentials for
                    // rays reflected from triangles with degenerate
                    // parameterizations are still reasonable.
                    Vector3f dn = Cross(Vector3f(mesh->n[v[2]] - mesh->n[v[0]]),
                                        Vector3f(mesh->n[v[1]] - mesh->n[v[0]]));
                    if (renderFromInstance)
                        dn = (*renderFromInstance)(dn);

                    if (LengthSquared(dn) == 0)
                        dndu = dndv = Normal3f(0, 0, 0);
                    else {
                        Vector3f dnu, dnv;
                        CoordinateSystem(dn, &dnu, &dnv);
                        dndu = Normal3f(dnu);
                        dndv = Normal3f(dnv);
                    }
                } else {
                    Float invDet = 1 / determinant;
                    dndu = DifferenceOfProducts(duv12[1], dn1, duv02[1], dn2) * invDet;
                    dndv = DifferenceOfProducts(duv02[0], dn2, duv12[0], dn1) * invDet;
                }
            } else
                dndu = dndv = Normal3f(0, 0, 0);

            isect.SetShadingGeometry(ns, ss, ts, dndu, dndv, true);
        }
        return isect;
    }

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const Point2f &u) const {
        // Get triangle vertices in _p0_, _p1_, and _p2_
        auto mesh = GetMesh();
        const int *v = &mesh->vertexIndices[3 * triIndex];
        const Point3f &p0 = mesh->p[v[0]], &p1 = mesh->p[v[1]];
        const Point3f &p2 = mesh->p[v[2]];

        // Sample point on triangle uniformly by area
        pstd::array<Float, 3> b = SampleUniformTriangle(u);
        Point3f p = b[0] * p0 + b[1] * p1 + b[2] * p2;

        // Compute surface normal for sampled point on triangle
        Normal3f n = Normalize(Normal3f(Cross(p1 - p0, p2 - p0)));
        // Ensure correct orientation of the geometric normal; follow the same
        // approach as was used in Triangle::Intersect().
        if (mesh->n != nullptr) {
            Normal3f ns(b[0] * mesh->n[v[0]] + b[1] * mesh->n[v[1]] +
                        (1 - b[0] - b[1]) * mesh->n[v[2]]);
            n = FaceForward(n, ns);
        } else if (mesh->reverseOrientation ^ mesh->transformSwapsHandedness)
            n *= -1;

        // Compute error bounds for sampled point on triangle
        Point3f pAbsSum = Abs(b[0] * p0) + Abs(b[1] * p1) + Abs((1 - b[0] - b[1]) * p2);
        Vector3f pError = Vector3f(gamma(6) * pAbsSum);

        Point3fi pi = Point3fi(p, pError);
        return ShapeSample{Interaction(pi, n), 1 / Area()};
    }

    PBRT_CPU_GPU
    Float PDF(const Interaction &) const { return 1 / Area(); }

    // The spherical sampling code has trouble with both very small and very
    // large triangles (on the hemisphere); fall back to uniform area sampling
    // in these cases. In the first case, there is presumably not a lot of
    // contribution from the emitter due to its subtending a small solid angle.
    // In the second, BSDF sampling should be the much better sampling strategy
    // anyway.
    static constexpr Float MinSphericalSampleArea = 1e-4;
    static constexpr Float MaxSphericalSampleArea = 6.28;

    // Note: much of this method---other than the call to the sampling function
    // and the check about how to sample---is shared with the other
    // Triangle::Sample() routine.
    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const ShapeSampleContext &ctx,
                                       const Point2f &uo) const {
        // Get triangle vertices in _p0_, _p1_, and _p2_
        auto mesh = GetMesh();
        const int *v = &mesh->vertexIndices[3 * triIndex];
        const Point3f &p0 = mesh->p[v[0]], &p1 = mesh->p[v[1]];
        const Point3f &p2 = mesh->p[v[2]];

        Float sa = SolidAngle(ctx.p());
        if (sa < MinSphericalSampleArea || sa > MaxSphericalSampleArea) {
            // From Shape::Sample().
            pstd::optional<ShapeSample> ss = Sample(uo);
            if (!ss)
                return {};
            ss->intr.time = ctx.time;
            Vector3f wi = ss->intr.p() - ctx.p();
            if (LengthSquared(wi) == 0)
                return {};
            else {
                wi = Normalize(wi);
                // Convert from area measure, as returned by the Sample() call
                // above, to solid angle measure.
                ss->pdf *=
                    DistanceSquared(ctx.p(), ss->intr.p()) / AbsDot(ss->intr.n, -wi);
                if (std::isinf(ss->pdf))
                    return {};
            }
            return ss;
        }

        Float pdf = 1;
        Point2f u = uo;
        if (ctx.ns != Normal3f(0, 0, 0)) {
            Point3f rp = ctx.p();
            Vector3f wi[3] = {Normalize(p0 - rp), Normalize(p1 - rp), Normalize(p2 - rp)};
            // (0,0) -> p1, (1,0) -> p1, (0,1) -> p0, (1,1) -> p2
            pstd::array<Float, 4> w =
                pstd::array<Float, 4>{std::max<Float>(0.01, AbsDot(ctx.ns, wi[1])),
                                      std::max<Float>(0.01, AbsDot(ctx.ns, wi[1])),
                                      std::max<Float>(0.01, AbsDot(ctx.ns, wi[0])),
                                      std::max<Float>(0.01, AbsDot(ctx.ns, wi[2]))};
            u = SampleBilinear(u, w);
            DCHECK(u[0] >= 0 && u[0] < 1 && u[1] >= 0 && u[1] < 1);
            pdf *= BilinearPDF(u, w);
        }
        Float triPDF;
        pstd::array<Float, 3> b =
            SampleSphericalTriangle({p0, p1, p2}, ctx.p(), u, &triPDF);
        if (triPDF == 0)
            return {};
        pdf *= triPDF;

        // Compute surface normal for sampled point on triangle
        Normal3f n = Normalize(Normal3f(Cross(p1 - p0, p2 - p0)));
        // Ensure correct orientation of the geometric normal; follow the same
        // approach as was used in Triangle::Intersect().
        if (mesh->n != nullptr) {
            Normal3f ns(b[0] * mesh->n[v[0]] + b[1] * mesh->n[v[1]] +
                        b[2] * mesh->n[v[2]]);
            n = FaceForward(n, ns);
        } else if (mesh->reverseOrientation ^ mesh->transformSwapsHandedness)
            n *= -1;

        // Compute error bounds for sampled point on triangle
        Point3f ps = b[0] * p0 + b[1] * p1 + b[2] * p2;
        Point3f pAbsSum = Abs(b[0] * p0) + Abs(b[1] * p1) + Abs(b[2] * p2);
        Vector3f pError = gamma(6) * Vector3f(pAbsSum.x, pAbsSum.y, pAbsSum.z);
        Point3fi pi = Point3fi(ps, pError);

        return ShapeSample{Interaction(pi, n, ctx.time), pdf};
    }

    PBRT_CPU_GPU
    Float PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const {
        Float sa = SolidAngle(ctx.p());
        if (sa < MinSphericalSampleArea || sa > MaxSphericalSampleArea) {
            // From Shape::PDF()
            // Intersect sample ray with area light geometry
            Ray ray = ctx.SpawnRay(wi);
            pstd::optional<ShapeIntersection> si = Intersect(ray);
            if (!si)
                return 0;

            // Convert light sample weight to solid angle measure
            Float pdf = DistanceSquared(ctx.p(), si->intr.p()) /
                        (AbsDot(si->intr.n, -wi) * Area());
            if (std::isinf(pdf))
                pdf = 0.f;
            return pdf;
        }

        if (!IntersectP(ctx.SpawnRay(wi), Infinity))
            return 0;

        Float pdf = 1 / sa;
        if (ctx.ns != Normal3f(0, 0, 0)) {
            // Get triangle vertices in _p0_, _p1_, and _p2_
            auto mesh = GetMesh();
            const int *v = &mesh->vertexIndices[3 * triIndex];
            const Point3f &p0 = mesh->p[v[0]], &p1 = mesh->p[v[1]];
            const Point3f &p2 = mesh->p[v[2]];

            Point3f rp = ctx.p();
            Vector3f wit[3] = {Normalize(p0 - rp), Normalize(p1 - rp),
                               Normalize(p2 - rp)};
            pstd::array<Float, 4> w =
                pstd::array<Float, 4>{std::max<Float>(0.01, AbsDot(ctx.ns, wit[1])),
                                      std::max<Float>(0.01, AbsDot(ctx.ns, wit[1])),
                                      std::max<Float>(0.01, AbsDot(ctx.ns, wit[0])),
                                      std::max<Float>(0.01, AbsDot(ctx.ns, wit[2]))};

            Point2f u = InvertSphericalTriangleSample({p0, p1, p2}, rp, wi);
            pdf *= BilinearPDF(u, w);
        }

        return pdf;
    }

    // Returns the solid angle subtended by the triangle w.r.t. the given
    // reference point p.
    PBRT_CPU_GPU
    Float SolidAngle(const Point3f &p, int = 0 /*nSamples: unused...*/) const {
        // Project the vertices into the unit sphere around p.
        auto mesh = GetMesh();
        const int *v = &mesh->vertexIndices[3 * triIndex];
        Vector3f a = Normalize(mesh->p[v[0]] - p);
        Vector3f b = Normalize(mesh->p[v[1]] - p);
        Vector3f c = Normalize(mesh->p[v[2]] - p);

        return SphericalTriangleArea(a, b, c);
    }

  private:
    // Triangle Private Methods
    PBRT_CPU_GPU
    const TriangleMesh *&GetMesh() const {
#ifdef PBRT_IS_GPU_CODE
        return (*allTriangleMeshesGPU)[meshIndex];
#else
        return (*allMeshes)[meshIndex];
#endif
    }

    PBRT_CPU_GPU
    pstd::array<Point2f, 3> GetUVs() const {
        auto mesh = GetMesh();
        if (mesh->uv) {
            const int *v = &mesh->vertexIndices[3 * triIndex];
            return {mesh->uv[v[0]], mesh->uv[v[1]], mesh->uv[v[2]]};
        } else
            return {Point2f(0, 0), Point2f(1, 0), Point2f(1, 1)};
    }

    // Triangle Private Members
    int meshIndex = -1, triIndex = -1;
    static pstd::vector<const TriangleMesh *> *allMeshes;
};

// CurveType Definition
enum class CurveType { Flat, Cylinder, Ribbon };

std::string ToString(CurveType type);

// CurveCommon Definition
struct CurveCommon {
    // CurveCommon Public Methods
    CurveCommon(pstd::span<const Point3f> c, Float w0, Float w1, CurveType type,
                pstd::span<const Normal3f> norm, const Transform *renderFromObject,
                const Transform *objectFromRender, bool reverseOrientation);

    std::string ToString() const;

    // CurveCommon Public Members
    CurveType type;
    Point3f cpObj[4];
    Float width[2];
    Normal3f n[2];
    Float normalAngle, invSinNormalAngle;
    const Transform *renderFromObject, *objectFromRender;
    bool reverseOrientation, transformSwapsHandedness;
};

// Curve Definition
class Curve {
  public:
    // Curve Public Methods
    static pstd::vector<ShapeHandle> Create(const Transform *renderFromObject,
                                            const Transform *objectFromRender,
                                            bool reverseOrientation,
                                            const ParameterDictionary &parameters,
                                            const FileLoc *loc, Allocator alloc);

    PBRT_CPU_GPU
    Bounds3f Bounds() const;
    pstd::optional<ShapeIntersection> Intersect(const Ray &ray, Float tMax) const;
    bool IntersectP(const Ray &ray, Float tMax) const;
    PBRT_CPU_GPU
    Float Area() const;

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const Point2f &u) const;
    PBRT_CPU_GPU
    Float PDF(const Interaction &) const;

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const ShapeSampleContext &ctx,
                                       const Point2f &u) const;
    PBRT_CPU_GPU
    Float PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const;

    PBRT_CPU_GPU
    bool OrientationIsReversed() const { return common->reverseOrientation; }
    PBRT_CPU_GPU
    bool TransformSwapsHandedness() const { return common->transformSwapsHandedness; }

    std::string ToString() const;

    Curve(const CurveCommon *common, Float uMin, Float uMax)
        : common(common), uMin(uMin), uMax(uMax) {}

    PBRT_CPU_GPU
    DirectionCone NormalBounds() const { return DirectionCone::EntireSphere(); }

  private:
    // Curve Private Methods
    bool intersect(const Ray &r, Float tMax, pstd::optional<ShapeIntersection> *si) const;
    bool recursiveIntersect(const Ray &r, Float tMax, pstd::span<const Point3f> cp,
                            const Transform &ObjectFromRay, Float u0, Float u1, int depth,
                            pstd::optional<ShapeIntersection> *si) const;

    // Curve Private Members
    const CurveCommon *common;
    Float uMin, uMax;
};

// BilinearPatch Declarations
#if defined(PBRT_BUILD_GPU_RENDERER) && defined(__CUDACC__)
extern PBRT_GPU pstd::vector<const BilinearPatchMesh *> *allBilinearMeshesGPU;
#endif

// BilinearIntersection Definition
struct BilinearIntersection {
    Point2f uv;
    Float t;
    std::string ToString() const;
};

// BilinearPatch Definition
class BilinearPatch {
  public:
    // BilinearPatch Public Methods
    BilinearPatch(int meshIndex, int blpIndex);

    static void Init(Allocator alloc);

    static BilinearPatchMesh *CreateMesh(const Transform *renderFromObject,
                                         bool reverseOrientation,
                                         const ParameterDictionary &parameters,
                                         const FileLoc *loc, Allocator alloc);

    static pstd::vector<ShapeHandle> CreatePatches(const BilinearPatchMesh *mesh,
                                                   Allocator alloc);

    PBRT_CPU_GPU
    Bounds3f Bounds() const;

    PBRT_CPU_GPU
    pstd::optional<ShapeIntersection> Intersect(const Ray &ray,
                                                Float tMax = Infinity) const;

    PBRT_CPU_GPU
    bool IntersectP(const Ray &ray, Float tMax = Infinity) const;

    PBRT_CPU_GPU
    Float Area() const;

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const ShapeSampleContext &ctx,
                                       const Point2f &u) const;

    PBRT_CPU_GPU
    Float PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const;

    PBRT_CPU_GPU
    pstd::optional<ShapeSample> Sample(const Point2f &u) const;

    PBRT_CPU_GPU
    Float PDF(const Interaction &) const;

    PBRT_CPU_GPU
    DirectionCone NormalBounds() const;

    std::string ToString() const;

    PBRT_CPU_GPU
    bool OrientationIsReversed() const { return GetMesh()->reverseOrientation; }

    PBRT_CPU_GPU
    bool TransformSwapsHandedness() const { return GetMesh()->transformSwapsHandedness; }

    PBRT_CPU_GPU
    static pstd::optional<BilinearIntersection> Intersect(const Ray &ray, Float tMax,
                                                          const Point3f &p00,
                                                          const Point3f &p10,
                                                          const Point3f &p01,
                                                          const Point3f &p11) {
        Vector3f qn = Cross(p10 - p00, p01 - p11);
        Vector3f e10 = p10 - p00;  // p01------u--------p11
        Vector3f e11 = p11 - p10;  // |                   |
        Vector3f e00 = p01 - p00;  // v e00           e11 v
        // |        e10        |
        // p00------u--------p10
        Vector3f q00 = p00 - ray.o;
        Vector3f q10 = p10 - ray.o;
        Float a = Dot(Cross(q00, ray.d), e00);  // the equation is
        Float c = Dot(qn, ray.d);               // a + b u + c u^2
        Float b = Dot(Cross(q10, ray.d), e11);  // first compute a+b+c
        b -= a + c;                             // and then b
        Float det = b * b - 4 * a * c;
        if (det < 0)
            return {};
        det = std::sqrt(det);
        Float u1, u2;          // two roots (u parameter)
        Float t = tMax, u, v;  // need solution for the smallest t > 0
        if (c == 0) {          // if c == 0, it is a trapezoid
            u1 = -a / b;
            u2 = -1;                            // and there is only one root
        } else {                                // (c != 0 in Stanford models)
            u1 = (-b - copysignf(det, b)) / 2;  // numerically "stable" root
            u2 = a / u1;                        // Viete's formula for u1*u2
            u1 /= c;
        }
        if (0 <= u1 && u1 <= 1) {              // is it inside the patch?
            Vector3f pa = Lerp(u1, q00, q10);  // point on edge e10 (Figure 8.4)
            Vector3f pb = Lerp(u1, e00, e11);  // it is, actually, pb - pa
            Vector3f n = Cross(ray.d, pb);
            det = Dot(n, n);
            n = Cross(n, pa);
            Float t1 = Dot(n, pb);
            Float v1 = Dot(n, ray.d);              // no need to check t1 < t
            if (t1 > 0 && 0 <= v1 && v1 <= det) {  // if t1 > ray.tmax,
                t = t1 / det;
                u = u1;
                v = v1 / det;  // it will be rejected
            }                  // in rtPotentialIntersection
        }
        if (0 <= u2 && u2 <= 1) {              // it is slightly different,
            Vector3f pa = Lerp(u2, q00, q10);  // since u1 might be good
            Vector3f pb = Lerp(u2, e00, e11);  // and we need 0 < t2 < t1
            Vector3f n = Cross(ray.d, pb);
            det = Dot(n, n);
            n = Cross(n, pa);
            Float t2 = Dot(n, pb) / det;
            Float v2 = Dot(n, ray.d);
            if (0 <= v2 && v2 <= det && t > t2 && t2 > 0) {
                t = t2;
                u = u2;
                v = v2 / det;
            }
        }

        // TODO: reject hits with sufficiently small t that we're not sure.

        if (t >= tMax)
            return {};

        return BilinearIntersection{{u, v}, t};
    }

    PBRT_CPU_GPU
    static SurfaceInteraction InteractionFromIntersection(
        const BilinearPatchMesh *mesh, int patchIndex, const Point2f &uvHit, Float time,
        const Vector3f &wo, pstd::optional<Transform> renderFromInstance = {}) {
        const int *v = &mesh->vertexIndices[4 * patchIndex];
        Point3f p00 = mesh->p[v[0]], p10 = mesh->p[v[1]], p01 = mesh->p[v[2]],
                p11 = mesh->p[v[3]];

        if (renderFromInstance) {
            p00 = (*renderFromInstance)(p00);
            p10 = (*renderFromInstance)(p10);
            p01 = (*renderFromInstance)(p01);
            p11 = (*renderFromInstance)(p11);
        }

        Point3f pHit = Lerp(uvHit[0], Lerp(uvHit[1], p00, p01), Lerp(uvHit[1], p10, p11));

        Vector3f dpdu = Lerp(uvHit[1], p10, p11) - Lerp(uvHit[1], p00, p01);
        Vector3f dpdv = Lerp(uvHit[0], p01, p11) - Lerp(uvHit[0], p00, p10);

        // Interpolate texture coordinates, if provided
        Point2f uv = uvHit;
        if (mesh->uv != nullptr) {
            const Point2f &uv00 = mesh->uv[v[0]];
            const Point2f &uv10 = mesh->uv[v[1]];
            const Point2f &uv01 = mesh->uv[v[2]];
            const Point2f &uv11 = mesh->uv[v[3]];

            Float dsdu =
                -uv00[0] + uv10[0] + uv[1] * (uv00[0] - uv01[0] - uv10[0] + uv11[0]);
            Float dsdv =
                -uv00[0] + uv01[0] + uv[0] * (uv00[0] - uv01[0] - uv10[0] + uv11[0]);
            Float dtdu =
                -uv00[1] + uv10[1] + uv[1] * (uv00[1] - uv01[1] - uv10[1] + uv11[1]);
            Float dtdv =
                -uv00[1] + uv01[1] + uv[0] * (uv00[1] - uv01[1] - uv10[1] + uv11[1]);

            Float duds = std::abs(dsdu) < 1e-8f ? 0 : 1 / dsdu;
            Float dvds = std::abs(dsdv) < 1e-8f ? 0 : 1 / dsdv;
            Float dudt = std::abs(dtdu) < 1e-8f ? 0 : 1 / dtdu;
            Float dvdt = std::abs(dtdv) < 1e-8f ? 0 : 1 / dtdv;

            // actually this is st (and confusing)
            uv = Lerp(uv[0], Lerp(uv[1], uv00, uv01), Lerp(uv[1], uv10, uv11));

            // dpds = dpdu * duds + dpdv * dvds, etc
            // duds = 1/dsdu
            Vector3f dpds = dpdu * duds + dpdv * dvds;
            Vector3f dpdt = dpdu * dudt + dpdv * dvdt;

            // These end up as zero-vectors of the mapping is degenerate...
            if (Cross(dpds, dpdt) != Vector3f(0, 0, 0)) {
                // Make sure the normal is in the same hemisphere...
                if (Dot(Cross(dpdu, dpdv), Cross(dpds, dpdt)) < 0)
                    dpdt = -dpdt;

                CHECK_GE(Dot(Normalize(Cross(dpdu, dpdv)), Normalize(Cross(dpds, dpdt))),
                         -1e-3);
                dpdu = dpds;
                dpdv = dpdt;
            }
        }

        // Compute coefficients for fundamental forms
        Float E = Dot(dpdu, dpdu);
        Float F = Dot(dpdu, dpdv);
        Float G = Dot(dpdv, dpdv);
        Vector3f N = Normalize(Cross(dpdu, dpdv));
        Float e = 0;  // 2nd derivative d2p/du2 == 0
        Vector3f d2Pduv(p00.x - p01.x - p10.x + p11.x, p00.y - p01.y - p10.y + p11.y,
                        p00.z - p01.z - p10.z + p11.z);
        Float f = Dot(N, d2Pduv);
        Float g = 0;  // samesies

        // Compute $\dndu$ and $\dndv$ from fundamental form coefficients
        Float EGF2 = DifferenceOfProducts(E, G, F, F);
        Normal3f dndu, dndv;
        if (EGF2 != 0) {
            Float invEGF2 = 1 / EGF2;
            dndu = Normal3f(DifferenceOfProducts(f, F, e, G) * invEGF2 * dpdu +
                            DifferenceOfProducts(e, F, f, E) * invEGF2 * dpdv);
            dndv = Normal3f(DifferenceOfProducts(g, F, f, G) * invEGF2 * dpdu +
                            DifferenceOfProducts(f, F, g, E) * invEGF2 * dpdv);
        }

        // Two lerps; each is gamma(3). TODO: double check.
        Vector3f pError =
            gamma(6) * Vector3f(Max(Max(Abs(p00), Abs(p10)), Max(Abs(p01), Abs(p11))));

        // Initialize _SurfaceInteraction_ from parametric information
        int faceIndex = mesh->faceIndices != nullptr ? mesh->faceIndices[patchIndex] : 0;
        Point3fi pe(pHit, pError);
        SurfaceInteraction isect(
            pe, uv, wo, dpdu, dpdv, dndu, dndv, time,
            mesh->reverseOrientation ^ mesh->transformSwapsHandedness, faceIndex);

        if (mesh->n != nullptr) {
            Normal3f n00 = mesh->n[v[0]], n10 = mesh->n[v[1]], n01 = mesh->n[v[2]],
                     n11 = mesh->n[v[3]];
            if (renderFromInstance) {
                n00 = (*renderFromInstance)(n00);
                n10 = (*renderFromInstance)(n10);
                n01 = (*renderFromInstance)(n01);
                n11 = (*renderFromInstance)(n11);
            }

            // TODO: should these be computed using normalized normals?
            Normal3f dndu = Lerp(uvHit[1], n10, n11) - Lerp(uvHit[1], n00, n01);
            Normal3f dndv = Lerp(uvHit[0], n01, n11) - Lerp(uvHit[0], n00, n10);

            Normal3f ns =
                Lerp(uvHit[0], Lerp(uvHit[1], n00, n01), Lerp(uvHit[1], n10, n11));
            if (LengthSquared(ns) > 0) {
                ns = Normalize(ns);
                Normal3f n = Normal3f(Normalize(isect.n));
                Vector3f axis = Cross(Vector3f(n), Vector3f(ns));
                if (LengthSquared(axis) > 1e-14) {
                    axis = Normalize(axis);
                    // The shading normal is different enough.
                    //
                    // Don't worry about if ns == -n; that, too, is handled
                    // naturally by the following.
                    //
                    // Rotate dpdu and dpdv around the axis perpendicular to the
                    // plane defined by n and ns by the angle between them ->
                    // their cross product will equal ns.
                    Float cosTheta = Dot(n, ns),
                          sinTheta = SafeSqrt(1 - cosTheta * cosTheta);
                    Transform r = Rotate(sinTheta, cosTheta, axis);
                    Vector3f sdpdu = r(dpdu), sdpdv = r(dpdv);

                    // Gram-Schmidt to ensure that Dot(sdpdu, ns) is
                    // basically zero.  (Otherwise a CHECK in the Frame
                    // constructor can end up hitting...)
                    sdpdu -= Dot(sdpdu, ns) * Vector3f(ns);
                    isect.SetShadingGeometry(ns, sdpdu, sdpdv, dndu, dndv, true);
                }
            }
        }

        return isect;
    }

  private:
    static constexpr Float MinSphericalSampleArea = 1e-4;

    PBRT_CPU_GPU
    bool IsQuad() const;

    PBRT_CPU_GPU
    const BilinearPatchMesh *&GetMesh() const {
#ifdef PBRT_IS_GPU_CODE
        return (*allBilinearMeshesGPU)[meshIndex];
#else
        return (*allMeshes)[meshIndex];
#endif
    }

    // BilinearPatch Private Data
    int meshIndex, blpIndex;
    Float area;

    static pstd::vector<const BilinearPatchMesh *> *allMeshes;
};

inline Bounds3f ShapeHandle::Bounds() const {
    auto cwb = [&](auto ptr) { return ptr->Bounds(); };
    return Dispatch(cwb);
}

inline pstd::optional<ShapeIntersection> ShapeHandle::Intersect(const Ray &ray,
                                                                Float tMax) const {
    auto intr = [&](auto ptr) { return ptr->Intersect(ray, tMax); };
    return Dispatch(intr);
}

inline bool ShapeHandle::IntersectP(const Ray &ray, Float tMax) const {
    auto intr = [&](auto ptr) { return ptr->IntersectP(ray, tMax); };
    return Dispatch(intr);
}

inline Float ShapeHandle::Area() const {
    auto area = [&](auto ptr) { return ptr->Area(); };
    return Dispatch(area);
}

inline pstd::optional<ShapeSample> ShapeHandle::Sample(const Point2f &u) const {
    auto sample = [&](auto ptr) { return ptr->Sample(u); };
    return Dispatch(sample);
}

inline Float ShapeHandle::PDF(const Interaction &in) const {
    auto pdf = [&](auto ptr) { return ptr->PDF(in); };
    return Dispatch(pdf);
}

inline pstd::optional<ShapeSample> ShapeHandle::Sample(const ShapeSampleContext &ctx,
                                                       const Point2f &u) const {
    auto sample = [&](auto ptr) { return ptr->Sample(ctx, u); };
    return Dispatch(sample);
}

inline Float ShapeHandle::PDF(const ShapeSampleContext &ctx, const Vector3f &wi) const {
    auto pdf = [&](auto ptr) { return ptr->PDF(ctx, wi); };
    return Dispatch(pdf);
}

inline DirectionCone ShapeHandle::NormalBounds() const {
    auto nb = [&](auto ptr) { return ptr->NormalBounds(); };
    return Dispatch(nb);
}

}  // namespace pbrt

#endif  // PBRT_SHAPES_H
