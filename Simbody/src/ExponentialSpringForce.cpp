/*-----------------------------------------------------------------------------
                               Simbody(tm)
-------------------------------------------------------------------------------
 Copyright (c) 2021 Authors.
 Authors: Frank C. Anderson
 Contributors:

 Licensed under the Apache License, Version 2.0 (the "License"); you may
 not use this file except in compliance with the License. You may obtain a
 copy of the License at http://www.apache.org/licenses/LICENSE-2.0.

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 ----------------------------------------------------------------------------*/


#include "SimTKcommon.h"
#include "simbody/internal/SimbodyMatterSubsystem.h"
#include "simbody/internal/ForceSubsystem.h"
#include "simbody/internal/ForceSubsystemGuts.h"
#include "simbody/internal/ExponentialSpringForce.h"


namespace SimTK {

//=============================================================================
// Flag for managing SlidingDot.
//=============================================================================
enum Transition {
    Hold = 0,   // Wait for a transition condition to be met.
    Decay = 1,  // Decay all the way to fixed (Sliding = 0).
    Rise = 2    // Rise all the way to full slipping (Sliding = 1).
};


//=============================================================================
// Struct ExponentialSpringData
//=============================================================================
/** ExponentialSpringData is an internal data structure used by the
implementation class ExponentialSpringForceImpl to store and retrieve
important quantities kept in the State's data cache.

Note - As originally coded, users could access class ExponentialSpringData
directly via a const reference. To make the exponential spring code less
brittle, however, class ExponentialSpringData was migrated to being an
internal data structure that is accessed directly only by class
ExponentialSpringForceImpl. The data stored in ExponentialSpringData can
now only be accessed by the end user via conventional assessor methods
in class ExponentialSpringForce.

Even though ExponentialSpringData is now used only internally, the
Doxygen-compliant comments originally written for the class might be helpful
to developers. I therefore kept the comments. They follow below:

ExponentialSpringData is a helper class that is used to store key
quantities associated with the ExponentialSpringForce Subsystem during a
simulation. An instance of this class serves as the data Cache Entry for the
ExponentialSpringForceImpl Subsystem. All of its member variables are
guaranteed to be calculated and set once the System has been realized
to Stage::Dynamics.

To understand what the quantities organized by this class represent, a basic
description of the contact problem that is solved, along with a description
of coordinate frame conventions, will be helpful.

Class ExponentialSpringForce computes and applies a contact force at a
specified point on a MobilizedBody (i.e., a Station) due to interaction of
that point with a specified contact plane. That plane is typically used to
model interactions with a floor, but need not be limited to this use case.
The contact plane can be rotated and displaced relative to the ground frame
and so can be used to model a wall or ramp, for example.

%Contact force computations are carried out in the frame of the contact plane.
The positive y-axis of the contact frame defines the normal of the
contact plane. The positive y-axis is the axis along which a repelling
normal force (modeled using an exponential) is applied. The x-axis and
z-axis of the contact frame are tangent to the contact plane. The friction
force will always lie in x-z plane.

Member variables with a "y" suffix (e.g., py, vy, or fy) indicate that
these quantities are directed normal to the contact plane.  Member varaibles
with an "xz" suffix (e.g., pxz, vxz, or fxz) indicate that these quantities
lie in the contact plane (or tangent to it) and are associated with the
friction force.

Member variables with a "_G" suffix are expressed in the Ground frame. Member
variables without a "_G" suffix are expressed the contact plane frame. */
struct ExponentialSpringData {
    /** Position of the body spring station in the ground frame. */
    Vec3 p_G;
    /** Velocity of the body spring station in the ground frame. */
    Vec3 v_G;
    /** Position of the body spring station in the frame of the contact
    plane. */
    Vec3 p;
    /** Velocity of the body spring station in the frame of the contact
    plane. */
    Vec3 v;
    /** Displacement of the body spring station normal to the floor expressed
    in the frame of the contact plane. */
    Real pz;
    /** Velocity of the body spring station normal to the contact plane
    expressed in the frame of the contact plane. */
    Real vz;
    /** Position of the body spring station projected onto the contact plane
    expressed in the frame of the contact plane. */
    Vec3 pxy;
    /** Velocity of the body spring station in the contact plane expressed in
    the frame of the contact plane. */
    Vec3 vxy;
    /** Elastic force in the normal direction. */
    Real fzElas;
    /** Damping force in the normal direction. */
    Real fzDamp;
    /** Total normal force expressed in the frame of the contact plane. */
    Real fz;
    /** Instantaneous coefficient of friction. */
    Real mu;
    /** Limit of the frictional force. */
    Real fxyLimit;
    /** Elastic frictional force expressed in the frame of the contact plane.*/
    Vec3 fricElas;
    /** Damping frictional force expressed in the frame of the contact plane.*/
    Vec3 fricDamp;
    /** Total frictional force (elastic + damping) expressed in the frame of
    the contact plane. */
    Vec3 fric;
    /** Magnitude of the frictional force. */
    Real fxy;
    /** Flag indicating if the frictional limit was exceeded. */
    bool limitReached;
    /** Resultant spring force (normal + friction) expressed in the floor
    frame. */
    Vec3 f;
    /** Resultant spring force (normal + friction) expressed in the ground
    frame. */
    Vec3 f_G;
};


//=============================================================================
// Class SpringZeroRecorder
//=============================================================================
/* SpringZeroRecorder provides the average speed of the spring zero during
a simulation. The spring zero is recorded at a time interval that matches the
characteristic rise and decay time of the Sliding state (tau). Only the last
two spring zeros are stored. The average he average speed is computed as
follows:

        ave speed = (p0_2 - p0_1) / (t2 - t1)

If only one spring zero has been recorded, there is not enough information
to compute a velocity.  In such a case a speed of 0.0 is returned.
*/
class SpringZeroRecorder : public PeriodicEventReporter {
public:
    SpringZeroRecorder(const ExponentialSpringForceImpl& spr,
        Real reportInterval);
    ~SpringZeroRecorder();
    void handleEvent(const State& state) const override;
    Real getSpeed() const;
private:
    const ExponentialSpringForceImpl& spr;
    Real* t;
    Vec3* p0;
}; // end of class SpringZeroRecorder


 //============================================================================
 // Class ExponentialSpringForceImpl
 //============================================================================
class ExponentialSpringForceImpl : public ForceSubsystem::Guts {
public:
// Constructor
ExponentialSpringForceImpl(const Transform& floor,
const MobilizedBody& body, const Vec3& station, Real mus, Real muk,
const ExponentialSpringParameters& params) :
ForceSubsystem::Guts("ExponentialSpringForce", "0.0.1"),
contactPlane(floor), body(body), station(station),
defaultMus(mus), defaultMuk(muk), defaultSprZero(Vec3(0., 0., 0.)) {
    // Check for valid static coefficient
    if(defaultMus < 0.0) defaultMus = 0.0;
    // Check for valid kinetic coefficient
    if(defaultMuk < 0.0) defaultMuk = 0.0;
    if(defaultMuk > defaultMus) defaultMuk = defaultMus;
    // Assign the parameters
    this->params = params;
    // Create the recorder
    //recorder = new SpringZeroRecorder(*this, this->params.getSlidingTimeConstant());
}

//-----------------------------------------------------------------------------
// Accessors
//-----------------------------------------------------------------------------
// SIMPLE
const Transform& getContactPlane() const { return contactPlane; }
const MobilizedBody& getBody() const { return body; }
const Vec3& getStation() const { return station; }

// TOPOLOGY PARAMETERS
const ExponentialSpringParameters& getParameters() const { return params; }
void setParameters(const ExponentialSpringParameters& params) {
    this->params = params;
    invalidateSubsystemTopologyCache(); }

// DATA CACHE
ExponentialSpringData& updData(const State& state) const {
    return Value<ExponentialSpringData>::updDowncast(updCacheEntry(state,
        indexData)); }
const ExponentialSpringData& getData(const State& state) const {
    return Value<ExponentialSpringData>::downcast(getCacheEntry(state,
        indexData)); }

// SLIDING STATE
Real getSliding(const State& state) const {
    return getZ(state)[indexZ]; }
Real getSlidingDotInCache(const State& state) const {
    return getZDot(state)[indexZ]; }
void updSlidingDotInCache(const State& state, Real slidingDot) const {
    // Does not invalidate the State.
    updZDot(state)[indexZ] = slidingDot; }

// SPRING ZERO
const Vec3& getSprZero(const State& state) const {
    return Value<Vec3>::downcast(getDiscreteVariable(state, indexSprZero)); }
Vec3& updSprZero(State& state) const {
    // Update occurs when the elastic force exceeds mu*N.
    return Value<Vec3>::updDowncast(updDiscreteVariable(state,indexSprZero)); }
Vec3 getSprZeroInCache(const State& state) const {
    return Value<Vec3>::downcast(
        getDiscreteVarUpdateValue(state, indexSprZero)); }
void updSprZeroInCache(const State& state, const Vec3& setpoint) const {
    // Will not invalidate the State.
    Value<Vec3>::updDowncast(updDiscreteVarUpdateValue(state, indexSprZero))
        = setpoint; }

// STATIC COEFFICENT OF FRICTION
const Real& getMuStatic(const State& state) const {
    return Value<Real>::downcast(getDiscreteVariable(state, indexMus)); }
void setMuStatic(State& state, Real mus) {
    // Keep mus greter than or equal to 0.0.
    if(mus < 0.0) mus = 0.0;
    Value<Real>::updDowncast(updDiscreteVariable(state, indexMus)) = mus;
    // Make sure muk is less than or equal to mus
    Real muk = getMuKinetic(state);
    if(muk > mus) {
        muk = mus;
        Value<Real>::updDowncast(updDiscreteVariable(state, indexMuk)) = muk;
    }
}

// KINETIC COEFFICENT OF FRICTION
const Real& getMuKinetic(const State& state) const {
    return Value<Real>::downcast(getDiscreteVariable(state, indexMuk)); }
void setMuKinetic(State& state, Real muk) const {
    // Keep muk >= to zero.
    if(muk < 0.0) muk = 0.0;
    Value<Real>::updDowncast(updDiscreteVariable(state, indexMuk)) = muk;
    // Make sure mus is greater than or equal to muk
    Real mus = getMuStatic(state);
    if(muk > mus) {
        Value<Real>::updDowncast(updDiscreteVariable(state, indexMus)) = muk;
    }
}

//-----------------------------------------------------------------------------
// ForceSubsystem Methods (overrides of virtual methods)
//-----------------------------------------------------------------------------
//_____________________________________________________________________________
// Clone
Subsystem::Guts*
cloneImpl() const override {
    return new ExponentialSpringForceImpl(contactPlane, body, station,
        defaultMus, defaultMuk, params);
}
//_____________________________________________________________________________
// Topology - allocate state variables and the data cache.
int
realizeSubsystemTopologyImpl(State& state) const override {
    // Coefficients of friction: mus and muk
    indexMus = allocateDiscreteVariable(state,
        Stage::Dynamics, new Value<Real>(defaultMus));
    indexMuk = allocateDiscreteVariable(state,
        Stage::Dynamics, new Value<Real>(defaultMuk));
    // SprZero
    indexSprZero =
        allocateAutoUpdateDiscreteVariable(state, Stage::Dynamics,
            new Value<Vec3>(defaultSprZero), Stage::Velocity);
    indexSprZeroInCache =
        getDiscreteVarUpdateIndex(state, indexSprZero);
    // Sliding
    Real initialValue = 1.0;
    Vector zInit(1, initialValue);
    indexZ = allocateZ(state, zInit);
    // Data
    indexData = allocateCacheEntry(state, Stage::Dynamics,
        new Value<ExponentialSpringData>(defaultData));
    return 0;
}
//_____________________________________________________________________________
// Dynamics - compute the forces modeled by this Subsystem.
//
// "params" references the configurable topology-stage parameters that govern
// the behavior of the exponential spring. These can be changed by the user,
// but the System must be realized at the Topology Stage after any such
// change.
//
// "data" references the key data that are calculated and stored as a
// Cache Entry when the System is realized at the Dynamics Stage.
// These data can be retrieved during a simulation by a reporter or handler,
// for example.
// 
// Variables without a suffix are expressed in the frame of the contact plane.
// 
// Variables with the _G suffix are expressed in the ground frame.
//
// Most every calculation happens in this one method, the calculations for
// setting SlidingDot being the notable exception. The conditions that must
// be met for transitioning to Sliding = 0 (fixed in place) include the
// acceleration of the body station. Therefore, SlidingDot are computed
// in 
int
realizeSubsystemDynamicsImpl(const State& state) const override {
    // Get current accumulated forces
    const MultibodySystem& system = MultibodySystem::downcast(getSystem());
    const SimbodyMatterSubsystem& matter = system.getMatterSubsystem();
    Vector_<SpatialVec>& forces_G =
        system.updRigidBodyForces(state, Stage::Dynamics);

    // Retrieve a writable reference to the data cache entry.
    // Many computed quantities are stored in the data cache.
    ExponentialSpringData& data = updData(state);

    // Get position and velocity of the spring station in Ground
    data.p_G = body.findStationLocationInGround(state, station);
    data.v_G = body.findStationVelocityInGround(state, station);

    // Transform the position and velocity into the contact frame.
    data.p = contactPlane.shiftBaseStationToFrame(data.p_G);
    data.v = contactPlane.xformBaseVecToFrame(data.v_G);
    if(abs(data.v[0]) < SignificantReal) data.v[0] = 0.0;
    if(abs(data.v[1]) < SignificantReal) data.v[1] = 0.0;

    // Resolve into normal (y) and tangential parts (xz plane)
    // Normal (perpendicular to contact plane)
    data.pz = data.p[2];
    data.vz = data.v[2];
    // Tangent (tangent to contact plane)
    data.pxy = data.p;    data.pxy[2] = 0.0;
    data.vxy = data.v;    data.vxy[2] = 0.0;

    // Get all the parameters upfront
    Real d0, d1, d2;
    params.getShapeParameters(d0, d1, d2);
    Real kvNorm = params.getNormalViscosity();
    Real kpFric = params.getElasticity();
    Real kvFric = params.getViscosity();
    Real kTau = 1.0 / params.getSlidingTimeConstant();
    Real vSettle = params.getSettleVelocity();

    // Normal Force (perpendicular to contact plane) -------------------------
    // Elastic Part
    data.fzElas = d1 * std::exp(-d2 * (data.pz - d0));
    // Damping Part
    data.fzDamp = -kvNorm * data.vz * data.fzElas;
    // Total
    data.fz = data.fzElas + data.fzDamp;
    // Don't allow the normal force to be negative or too large.
    // Note that conservation of energy will fail if bounds are enforced.
    // The upper limit can be justified as a crude model of yielding.
    data.fz = ClampAboveZero(data.fz, 100000.0);

    // Friction (in the plane of contact plane) ------------------------------
    // Get the sliding state, which is bounded by 0.0 and 1.0.
    Real sliding = getZ(state)[indexZ];
    if(sliding < 0.0) sliding = 0.0;
    else if(sliding > 1.0) sliding = 1.0;
    // Compute the maximum allowed frictional force based on the current
    // coefficient of friction.
    Real mus = getMuStatic(state);
    Real muk = getMuKinetic(state);
    data.mu = mus - sliding * (mus - muk);
    data.fxyLimit = data.mu * data.fz;
    // Get the SprZero from the State.
    Vec3 p0 = getSprZero(state);
    
    // 0.0 < Sliding < 1.0 (transitioning)
    // Friction is a combination of a linear spring and pure damping.
    // As Sliding --> 1.0, the elastic term --> 0.0
    // As Sliding --> 1.0, the damping term --> fricLimit

    // Sliding = 1.0 (sliding)
    // Friction is the result purely of damping (no elastic term).
    // To avoid numerical issues, the friction limit is set to zero when
    // mu*Fn (data.fxyLimit) is less than the constant SimTK::SignificantReal.
    // If the damping force is greater than data.fxyLimit, the damping force
    // is capped at data.fxyLimit.
    Vec3 fricDampSpr = -kvFric * data.vxy;
    Vec3 fricLimit;
    if(data.fxyLimit < SignificantReal) fricLimit = 0.0;
    else {
        fricLimit = fricDampSpr;
        if(fricLimit.norm() > data.fxyLimit)
            fricLimit = data.fxyLimit * fricLimit.normalize();
    }

    // Sliding = 0.0 (fixed in place)
    // Friction is modeled as a linear spring.
    // The elastic component prevents drift while maintaining good integrator
    // step sizes, at least compared to increasing the damping coefficient.
    data.limitReached = false;
    Vec3 fricElasSpr = -kpFric * (data.pxy - p0);
    Vec3 fricSpr = fricElasSpr + fricDampSpr;
    Real fxySpr = fricSpr.norm();
    if(fxySpr > data.fxyLimit) {
        data.limitReached = true;
        Real scale = data.fxyLimit / fxySpr;
        fricElasSpr *= scale;
        fricDampSpr *= scale;
        fricSpr = fricElasSpr + fricDampSpr;
    }

    // Blend the two extremes according to the Sliding state
    // As Sliding --> 1, damping dominates
    // As Sliding --> 0, the spring model dominates
    data.fricElas = fricElasSpr * (1.0 - sliding);
    data.fricDamp = fricDampSpr + (fricLimit - fricDampSpr) * sliding;
    data.fric = data.fricElas + data.fricDamp;
    data.fxy = data.fric.norm();

    // Update the spring zero
    p0 = data.pxy + data.fricElas / kpFric;
    p0[2] = 0.0;  // Make sure p0 lies in the contact plane.
    updSprZeroInCache(state, p0);
    markCacheValueRealized(state, indexSprZeroInCache);

    // Total spring force expressed in the frame of the Contact Plane.
    data.f = data.fric;     // The x and z components are friction.
    data.f[2] = data.fz;    // The y component is the normal force.

    // Transform the spring forces back to the Ground frame
    data.f_G = contactPlane.xformFrameVecToBase(data.f);

    // Apply the force
    body.applyForceToBodyPoint(state, station, data.f_G, forces_G);


/* SAVE.  First blending attempt.
This code scales the damping term down as sliding --> 1.0,
the opposite of what should happen.
The elastic term should be scaled down!
    // Raw Parts (parts before limits are applied)
    Vec3 fxyElasRaw = -kpFric * (data.pxy - p0);
    Vec3 fxyDampRaw = -kvFric * data.vxy;
    Vec3 fxyRaw = fxyElasRaw + fxyDampRaw;
    // Compute the Limit
    Vec3 fxyLimit(0.0);
    Real fxyRawMag = fxyRaw.norm();
    if(fxyRawMag > SignificantReal)
        fxyLimit = data.fxyLimit * fxyRaw.normalize();

    // Compute the ratio of the raw spring force size to the limit
    Real ratio = 0.0;
    if(data.fxyLimit < SignificantReal) ratio = 1.0;
    else ratio = fxyRawMag / data.fxyLimit;
    if(ratio > 1.0) ratio = 1.0;

    // Scale by the ratio
    // As ratio --> 1.0, fricElas --> fxyLimit and fricDamp --> 0.0
    // This does not change the direction of total friction force.
    data.fricElas = fxyElasRaw + (fxyLimit - fxyElasRaw) * ratio;
    data.fricDamp = fxyDampRaw * (1.0 - ratio);
    data.fric = data.fricElas + data.fricDamp;
    data.fxy = data.fric.norm();

    // Compute a new spring zero.
    Vec3 p0New = p0;
    if(ratio == 1.0) {
        p0New = data.pxy + data.fricElas / kpFric;
        p0New[2] = 0.0;
    }

    // Update the spring zero cache and mark the cache as realized.
    // Only place that the following two lines are called.
    updSprZeroInCache(state, p0New);
    markCacheValueRealized(state, indexSprZeroInCache);

    // Compute and update SlidingDot
    // SlidingDot should key off of average velocity.
    //Real vMag = data.vxy.norm();
    //Real slidingDot = 0.0;
    ////slidingDot = kTau * (Sigma(0.8,0.05,ratio) - sliding);
    //if((ratio>=1.0)&&(vMag>vSettle)) slidingDot = kTau * (1.0 - sliding);
    //else if((ratio<1.0)&&(vMag<vSettle)) slidingDot = -kTau * sliding;
    //slidingDot = 0.0;
    //updSlidingDotInCache(state, slidingDot);
    //std::cout << "t= " << state.getTime() << "  s= " << sliding << ", " << slidingDot << std::endl;
 */


/* SAVE - Very old code, prior to the blending idea.
    // If the spring is stretched beyond its limit, update the spring zero.
    // Note that no discontinuities in the friction force are introduced.
    // The spring zero is just made to be consistent with the limiting
    // frictional force.
    bool limitReached = false;
    if(fxyElas > data.fxyLimit) {
        limitReached = true;
        // Compute a new spring zero.
        data.fricElas = data.fxyLimit * data.fricElas.normalize();
        p0 = data.pxy + data.fricElas / kpFric;
        p0[2] = 0.0;
        // The damping part must be zero!
        data.fricDamp = Vec3(0.0, 0.0, 0.0);
    // spring zero does not need to be recalculated, but fricDamp = 0.0
    } else if(fxyElas == data.fxyLimit) {
        limitReached = true;
        data.fricDamp = Vec3(0.0, 0.0, 0.0);
    // calculate fricDamp
    } else {
        data.fricDamp = -kvFric * data.vxy;
    }
    // Total
    data.fric = data.fricElas + data.fricDamp;
    data.fxy = data.fric.norm();
    if(data.fxy > data.fxyLimit) {
        data.fxy = data.fxyLimit;
        data.fric = data.fxy * data.fric.normalize();
        data.fricDamp = data.fric - data.fricElas;
    }

    // Update value for the spring zero
    updSprZeroInCache(state, p0);
    markCacheValueRealized(state, indexSprZeroInCache);

    // Update SlidingDot
    Real vMag = data.vxy.norm();
    Real slidingDot = 0.0;
    if(limitReached && (vMag>vSettle))  slidingDot = kTau * (1.0 - sliding);
    else if(!limitReached && (vMag<=vSettle))  slidingDot = -kTau * sliding;
    //slidingDot = 0.0;
    updSlidingDotInCache(state, slidingDot);
*/

/* Repeat of what's above.  Just wanted the following lines up next to
the active code.

    // Total spring force expressed in the frame of the Contact Plane.
    data.f = data.fric;     // The x and z components are friction.
    data.f[2] = data.fz;    // The y component is the normal force.

    // Transform the spring forces back to the Ground frame
    data.f_G = contactPlane.xformFrameVecToBase(data.f);

    // Apply the force
    body.applyForceToBodyPoint(state, station, data.f_G, forces_G);
*/
    return 0;
}
//_____________________________________________________________________________
// Acceleration - compute and update the derivatives of continuous states.
int
realizeSubsystemAccelerationImpl(const State& state) const override {
    // Parameters
    Real kTau = 1.0 / params.getSlidingTimeConstant();
    Real vSettle = params.getSettleVelocity();
    
    // Current Sliding State
    Real sliding = getZ(state)[indexZ];
 
    // Writable reference to the data cache
    // Values are updated during System::realize(Stage::Dynamics) (see above)
    const ExponentialSpringData& data = getData(state);

    // Initialize SlidingDot
    Real slidingDot = 0.0;

    // Conditions for transitioning to "fixed" (Sliding --> 0.0)
    // Basically, static equilibrium.
    // 1. fxy < fxyLimit, AND
    // 2. |v| < vSettle  (Note that v must be small in ALL directions), AND
    // 3. |a| < aSettle  (Note that a must be small in ALL directions)
    Vec3 a = body.MobilizedBody::
        findStationAccelerationInGround(state, station);
    Real aMag = a.norm();
    if(!data.limitReached &&
        (data.vxy.norm() < 0.001) && (abs(data.vz) < 0.001))
        slidingDot = -kTau * sliding;

    // Conditions for transitioning to "sliding" (Sliding --> 1.0)
    // 1. limitReached = true, OR
    // 2. fz < SimTK::SignificantReal (when not "touching" contact plane)
    if(data.limitReached || (data.fz < SignificantReal))
        slidingDot = kTau * (1.0 - sliding);

    // Update
    //slidingDot = 0.0;
    updSlidingDotInCache(state, slidingDot);
    //std::cout << "t= " << state.getTime() << "  s= " << sliding << ", " << slidingDot << std::endl;
 
    return 0;
}
//_____________________________________________________________________________
// Potential Energy - calculate the potential energy stored in the spring.
// The System should be realized through Stage::Dynamics before a call to
// this method is made.
Real
calcPotentialEnergy(const State& state) const override {
    const MultibodySystem& system = MultibodySystem::downcast(getSystem());
    const ExponentialSpringData& data = getData(state);
    // Strain energy in the normal direction (exponential spring)
    Real d0, d1, d2;
    params.getShapeParameters(d0, d1, d2);
    double energy = data.fzElas / d2;
    // Strain energy in the tangent plane (friction spring)
    // Note that the updated spring zero (the one held in cache) needs to be
    // used, not the one in the state.
    // In the process of realizing to Stage::Dynamics, the spring zero is
    // changed when fxzElas > fxzLimit. This change is not reflected in the
    // state, just in the cache.
    Vec3 p0Cache = getSprZeroInCache(state);
    Vec3 r = data.pxy - p0Cache;
    energy += 0.5 * params.getElasticity() * r.norm() * r.norm();
    return energy;
}

//-----------------------------------------------------------------------------
// Utility and Static Methods
//-----------------------------------------------------------------------------
//_____________________________________________________________________________
// Project the body spring station onto the contact plane.
void
resetSprZero(State& state) const {
    // Realize through to the Position Stage
    const MultibodySystem& system = MultibodySystem::downcast(getSystem());
    system.realize(state, Stage::Position);
    // Get position of the spring station in the Ground frame
    Vec3 p_G = body.findStationLocationInGround(state, station);
    // Express the position in the contact plane.
    Vec3 p = contactPlane.shiftBaseStationToFrame(p_G);
    // Project onto the contact plane.
    p[2] = 0.0;
    // Update the spring zero
    updSprZero(state) = p;
}
//_____________________________________________________________________________
// Note - Not using this method, but keeping it around in case I want
// a reminder of how cache access works.
// Realize the SprZero Cache.
// There needs to be some assurance that the initial value of the SprZero
// is valid. Therefore, the first time a getSprZeroInCache() is called
// a call to realizeSprZeroCache() is also made.  Once the Cache for
// the SprZero has been realized, it may be repeatedly accessed with only
// the cost of the method call and the if statement.
void
realizeSprZeroCache(const State& state) const {
    if(isCacheValueRealized(state, indexSprZeroInCache)) return;
    Vec3 sprZero = getSprZero(state);
    Real time = state.getTime();
    sprZero[0] = 0.01 * time;
    sprZero[1] = 0.01 * time; 
    sprZero[2] = 0.0;
    updSprZeroInCache(state, sprZero);
    markCacheValueRealized(state, indexSprZeroInCache);
}
//_____________________________________________________________________________
// Clamp a value between zero and a maximum value.
static Real
ClampAboveZero(Real value, Real max) {
    if(value > max) return max;
    if(value < 0.0) return 0.0;
    return value;
}
//_____________________________________________________________________________
// Sigma - a function that transitions smoothly from 0.0 to 1.0 or
// from 1.0 to 0.0.
//
//   f(t) = 1.0 / {1.0 + exp[(t - t0) / tau]}
//   t0 - time about which the transition is centered.  F(t0) = 0.5.
//   tau - time constant modifying the rate of the transiton occurs.
//   tau < 0.0 generates a step up
//   tau > 0.0 generates a step down
//   A larger value of tau results in a more gradual transition.
//
// Step Up(negative tau)
//                    | f(t)
//                   1.0                         * ************
//                    |                  *
//                    |              *
//                   0.5 +
//                    |         *
//                    |    *
//  ***************---|-----------t0-------------------------  t
//                    |
//
// Step Down(positive tau)
//                    | f(t)
//  ***************  1.0
//                    |    *
//                    |         *
//                   0.5 +
//                    |               *
//                    |                   *
//  ------------------|-----------t0------------*************  t
//
static Real
Sigma(Real t0, Real tau, Real t) {
    Real x = (t - t0) / tau;
    Real s = 1.0 / (1.0 + std::exp(x));
    return s;
}

//-----------------------------------------------------------------------------
// Data Members
//-----------------------------------------------------------------------------
private:
    ExponentialSpringParameters params;
    ExponentialSpringData defaultData;
    //SpringZeroRecorder* recorder;
    Transform contactPlane;
    const MobilizedBody& body;
    Vec3 station;
    Real defaultMus;
    Real defaultMuk;
    Vec3 defaultSprZero;
    mutable DiscreteVariableIndex indexMus;
    mutable DiscreteVariableIndex indexMuk;
    mutable DiscreteVariableIndex indexSprZero;
    mutable CacheEntryIndex indexSprZeroInCache;
    mutable ZIndex indexZ;
    mutable CacheEntryIndex indexData;

};  // end of class ExponentialSpringForceImpl

} // namespace SimTK


using namespace SimTK;
using std::cout;
using std::endl;

//=============================================================================
// Class SpringZeroRecorder
//=============================================================================
/* SpringZeroRecorder provides the average speed of the spring zero during
a simulation. The spring zero is recorded at a time interval that matches the
characteristic rise and decay time of the Sliding state (tau). Only the last
two spring zeros are stored. The average he average speed is computed as
follows:

        ave speed = (p0_2 - p0_1) / (t2 - t1)

If only one spring zero has been recorded, there is not enough information
to compute a velocity.  In such a case a speed of 0.0 is returned.
*/
//_____________________________________________________________________________
SpringZeroRecorder::
SpringZeroRecorder(const ExponentialSpringForceImpl& spr, Real reportInterval)
        : PeriodicEventReporter(reportInterval), spr(spr) {
        t = new Real[2];
        p0 = new Vec3[2];
        t[0] = NaN;
        t[1] = NaN;
    }
//_____________________________________________________________________________
SpringZeroRecorder::
~SpringZeroRecorder() {
    delete t;
    delete p0;
}
//_____________________________________________________________________________
void
SpringZeroRecorder::
handleEvent(const State& state) const {
    t[0] = t[1];
    t[1] = state.getTime();

    p0[0] = p0[1];
    p0[1] = spr.getSprZero(state);
}
//_____________________________________________________________________________
Real
SpringZeroRecorder::getSpeed() const {
    Real dt = t[1] - t[0];
    if(isNaN(dt)) return 0.0;
    Real delta = (p0[1] - p0[0]).norm();
    return delta / dt;
}


//=============================================================================
// Class ExponentialSpringForce
//=============================================================================
//_____________________________________________________________________________
// Constructor.
ExponentialSpringForce::
ExponentialSpringForce(MultibodySystem& system,
    const Transform& contactPlane,
    const MobilizedBody& body, const Vec3& station,
    Real mus, Real muk, ExponentialSpringParameters params)
{
    adoptSubsystemGuts(
        new ExponentialSpringForceImpl(contactPlane, body, station,
            mus, muk, params));
    system.addForceSubsystem(*this);
    //getImpl().addSpringZeroRecorder(system);
}
//_____________________________________________________________________________
// Get the Transform specifying the location and orientation of the Contact
// Plane.
const Transform&
ExponentialSpringForce::
getContactPlane() const {
    return getImpl().getContactPlane();
}
//_____________________________________________________________________________
// Get the point on the body that interacts with the contact plane and at
// which the contact force is applied.
const MobilizedBody&
ExponentialSpringForce::
getBody() const {
    return getImpl().getBody();
}
//_____________________________________________________________________________
// Get the point on the body that interacts with the contact plane and at
// which the contact force is applied.
const Vec3&
ExponentialSpringForce::
getStation() const {
    return getImpl().getStation();
}

//_____________________________________________________________________________
// Set new parameters for this exponential spring.
//
// Note that the underlying implementation (ExponentialSpringForceImpl) owns
// its own ExponentialSpringParameters instance.  When this method is called,
// the underlying implementation sets its parameters equal to the parameters
// sent in through the argument list by calling the assignment operator:
//
//        ExponentialSpringForceImple::params = params
// 
// @see ExponentialSpringParameters for the list of parameters.
void
ExponentialSpringForce::
setParameters(const ExponentialSpringParameters& params) {
    updImpl().setParameters(params);
}
//_____________________________________________________________________________
// Get the current parameters for this exponential spring.
// @see ExponentialSpringParameters for the list of parameters.
const ExponentialSpringParameters&
ExponentialSpringForce::
getParameters() const {
    return getImpl().getParameters();
}

//_____________________________________________________________________________
// Set the static coefficient of fricition
void
ExponentialSpringForce::
setMuStatic(State& state, Real mus) {
    updImpl().setMuStatic(state, mus);
}
//_____________________________________________________________________________
// Get the static coefficient of fricition
Real
ExponentialSpringForce::
getMuStatic(const State& state) const {
    return getImpl().getMuStatic(state);
}

//_____________________________________________________________________________
// Set the kinetic coefficient of fricition
void
ExponentialSpringForce::
setMuKinetic(State& state, Real muk) {
    updImpl().setMuKinetic(state, muk);
}
//_____________________________________________________________________________
// Get the kinetic coefficient of fricition
Real
ExponentialSpringForce::
getMuKinetic(const State& state) const {
    return getImpl().getMuKinetic(state);
}
//_____________________________________________________________________________
// Get the value of the Sliding state.
Real
ExponentialSpringForce::
getSliding(const State& state) const {
    return getImpl().getSliding(state);
}

//_____________________________________________________________________________
// Reset the spring zero.
// This method sets the spring zero to the point on the contact plane that
// coincides with the Station that has been specified on the MobilizedBody
// for which this exponential spring was constructed.
void
ExponentialSpringForce::
resetSpringZero(State& state) const {
    getImpl().resetSprZero(state);
}

//-----------------------------------------------------------------------------
// Spring Data Accessor Methods
//-----------------------------------------------------------------------------
//_____________________________________________________________________________
// Get the elastic part of the normal force.
Vec3
ExponentialSpringForce::
getNormalForceElasticPart(const State& state, bool inGround) const {
    Vec3 fzElas(0.);
    fzElas[2] = getImpl().getData(state).fzElas;
    if(inGround) fzElas = getContactPlane().xformFrameVecToBase(fzElas);
    return fzElas;
}
//_____________________________________________________________________________
// Get the damping part of the normal force.
Vec3
ExponentialSpringForce::
getNormalForceDampingPart(const State& state, bool inGround) const {
    Vec3 fzDamp(0.);
    fzDamp[2] = getImpl().getData(state).fzDamp;
    if(inGround) fzDamp = getContactPlane().xformFrameVecToBase(fzDamp);
    return fzDamp;
}
//_____________________________________________________________________________
// Get the magnitude of the normal force.
Vec3
ExponentialSpringForce::
getNormalForce(const State& state, bool inGround) const {
    Vec3 fz(0.);
    fz[2] = getImpl().getData(state).fz;
    if(inGround) fz = getContactPlane().xformFrameVecToBase(fz);
    return fz;
}
//_____________________________________________________________________________
// Get the instantaneous coefficient of friction.
Real
ExponentialSpringForce::
getMu(const State& state) const {
    return getImpl().getData(state).mu;
}
//_____________________________________________________________________________
// Get the friction limit.
Real
ExponentialSpringForce::
getFrictionForceLimit(const State& state) const {
    return getImpl().getData(state).fxyLimit;
}
//_____________________________________________________________________________
// Get the elastic part of the friction force.
Vec3
ExponentialSpringForce::
getFrictionForceElasticPart(const State& state, bool inGround) const {
    Vec3 fricElas = getImpl().getData(state).fricElas;;
    if(inGround) fricElas = getContactPlane().xformFrameVecToBase(fricElas);
    return fricElas;
}
//_____________________________________________________________________________
// Get the elastic part of the friction force.
Vec3
ExponentialSpringForce::
getFrictionForceDampingPart(const State& state, bool inGround) const {
    Vec3 fricDamp = getImpl().getData(state).fricDamp;;
    if(inGround) fricDamp = getContactPlane().xformFrameVecToBase(fricDamp);
    return fricDamp;
}
//_____________________________________________________________________________
// Get the total friction force.
Vec3
ExponentialSpringForce::
getFrictionForce(const State& state, bool inGround) const {
    Vec3 fric = getImpl().getData(state).fric;;
    if(inGround) fric = getContactPlane().xformFrameVecToBase(fric);
    return fric;
}

//_____________________________________________________________________________
// Get the spring force applied to the MobilizedBody.
Vec3
ExponentialSpringForce::
getForce(const State& state, bool inGround) const {
    Vec3 force;
    if(inGround) {
        force = getImpl().getData(state).f_G;
    } else {
        force = getImpl().getData(state).f;
    }
    return force;
}
//_____________________________________________________________________________
// Get the position of the spring station.
Vec3
ExponentialSpringForce::
getStationPosition(const State& state, bool inGround) const {
    Vec3 pos_B = getStation();
    Vec3 pos_G = getBody().findStationLocationInGround(state, pos_B);
    if(inGround) return pos_G;
    Vec3 pos = getContactPlane().shiftBaseStationToFrame(pos_G);
    return pos;
}
//_____________________________________________________________________________
// Get the velocity of the spring station.
Vec3
ExponentialSpringForce::
getStationVelocity(const State& state, bool inGround) const {
    Vec3 pos_B = getStation();
    Vec3 vel_G = getBody().findStationVelocityInGround(state, pos_B);
    if(inGround) return vel_G;
    Vec3 vel = getContactPlane().xformBaseVecToFrame(vel_G);
    return vel;
}
//_____________________________________________________________________________
// Get the position of the spring zero.
// ? Should I be returning the spring zero that is stored in the Cache?
Vec3
ExponentialSpringForce::
getSpringZeroPosition(const State& state, bool inGround) const {
    Vec3 p0 = getImpl().getSprZero(state);
    if(inGround) {
        p0 = getContactPlane().shiftFrameStationToBase(p0);
    }
    return p0;
}



//-----------------------------------------------------------------------------
// Implmentation Accesssors
//-----------------------------------------------------------------------------
//_____________________________________________________________________________
// Get a reference to the underlying implementation that will allow changes
// to be made to underlying parameters and states.
ExponentialSpringForceImpl&
ExponentialSpringForce::
updImpl() {
    return dynamic_cast<ExponentialSpringForceImpl&>(updRep());
}
//_____________________________________________________________________________
// Get a reference to the underlying implementation that will allow
// access (but not change) to underlying parameters and states.
const ExponentialSpringForceImpl&
ExponentialSpringForce::
getImpl() const {
    return dynamic_cast<const ExponentialSpringForceImpl&>(getRep());
}


