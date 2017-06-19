/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license in the file COPYING
 * or http://www.opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file COPYING.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2017 Saso Kiselkov. All rights reserved.
 */

#include <string.h>
#include <stddef.h>

#if	IBM
#include <gl.h>
#elif	APL
#include <OpenGL/gl.h>
#else	/* LIN */
#include <GL/gl.h>
#endif	/* LIN */

#include <XPLMCamera.h>
#include <XPLMDisplay.h>
#include <XPLMScenery.h>
#include <XPLMUtilities.h>
#include <XPLMProcessing.h>

#include "assert.h"
#include "geom.h"
#include "list.h"
#include "dr.h"
#include "time.h"

#include "bp.h"

#define	STRAIGHT_STEER_RATE	40	/* degrees per second */
#define	TURN_STEER_RATE		10	/* degrees per second */
#define	STRAIGHT_SPEED		1.11	/* m/s [4 km/h, "walking speed"] */
#define	FAST_STRAIGHT_SPEED	7	/* m/s [~14 knots] */
#define	TURN_SPEED		0.55	/* m/s [2 km/h] */
#define	STRAIGHT_ACCEL		0.25	/* m/s^2 */
#define	TURN_ACCEL		0.25	/* m/s^2 */
#define	BRAKE_PEDAL_THRESH	0.1	/* brake pedal angle, 0..1 */
#define	FORCE_PER_TON		5000	/* max push force per ton, Newtons */
#define	BREAKAWAY_THRESH	0.1	/* m/s */
#define	SEG_TURN_MULT		0.9	/* leave 10% for oversteer */
#define	TURN_COMPLETE_THRESH	2	/* degrees */
#define	SPEED_COMPLETE_THRESH	0.05	/* m/s */

#define	DEBUG_PRINT_SEG(class, level, seg) \
	do { \
		if ((seg)->type == SEG_TYPE_STRAIGHT) { \
			dbg_log(class, level, "%.1f/%.1f/%.1f " \
			    "-(%s/%.1f)> %.1f/%.1f/%.1f", (seg)->start_pos.x, \
			    (seg)->start_pos.y, (seg)->start_hdg, \
			    (seg)->backward ? "B" : "S", (seg)->len, \
			    (seg)->end_pos.x, (seg)->end_pos.y, \
			    (seg)->end_hdg); \
		} else { \
			dbg_log(class, level, "%.1f/%.1f/%.1f " \
			    "-(%s/%.1f/%s)> %.1f/%.1f/%.1f", \
			    (seg)->start_pos.x, (seg)->start_pos.y, \
			    (seg)->start_hdg, (seg)->backward ? "B" : "S", \
			    (seg)->turn.r, (seg)->turn.right ? "R" : "L", \
			    (seg)->end_pos.x, (seg)->end_pos.y, \
			    (seg)->end_hdg); \
		} \
	} while (0)

typedef struct {
	double		mass;
	double		wheelbase;
	double		nw_z, main_z;
	double		max_nw_angle;
} acf_t;

typedef enum {
	SEG_TYPE_STRAIGHT,
	SEG_TYPE_TURN
} seg_type_t;

typedef struct {
	seg_type_t	type;

	vect2_t		start_pos;
	double		start_hdg;
	vect2_t		end_pos;
	double		end_hdg;

	/*
	 * A backward pushback segment looks like this:
	 *         ^^  (start_hdg)
	 *      ---++  (start_pos)
	 *      ^  ||
	 *      |  ||
	 * (s1) |  ||
	 *      |  ||
	 *      v  || (r)
	 *      ---||-----+
	 *          \\    | (r)    (end_hdg)
	 *            \\  |            |
	 *              ``=============<+ (end_pos)
	 *                |             |
	 *                |<----------->|
	 *                      (s2)
	 *
	 * A towing segment is similar, but the positions of the respective
	 * segments is reversed.
	 */
	bool_t		backward;
	union {
		double		len;	/* straight segment length (meters) */
		struct {
			double	r;	/* turn radius (meters) */
			bool_t	right;	/* turn center is right or left */
		} turn;
	};

	/*
	 * Flag indicating if the user placed this segment. Non-user-placed
	 * segments (up to the last user-placed segment) are deleted from
	 * the segment list.
	 */
	bool_t		user_placed;

	list_node_t	node;
} seg_t;

typedef struct {
	acf_t		acf;		/* our aircraft */

	vect2_t		cur_pos;	/* current position in meters */
	double		cur_hdg;	/* current heading in degrees */
	double		cur_spd;	/* current speed in m/s */
	double		cur_t;		/* current time in seconds */

	vect2_t		last_pos;	/* cur_pos from previous run */
	double		last_hdg;	/* cur_hdg from previous run */
	double		last_spd;	/* cur_spd from previous run */
	double		last_t;		/* cur_t from previous run */

	/* deltas from last_* to cur_* */
	vect2_t		d_pos;		/* delta from last_pos to cur_pos */
	double		d_hdg;		/* delta from last_hdg to cur_hdg */
	double		d_t;		/* delta time from last_t to cur_t */

	double		last_force;
	vect2_t		turn_c_pos;

	bool_t		stopping;	/* stopping at end of operation */
	bool_t		stopped;	/* stopped moving, waiting for pbrk */

	list_t		segs;
} bp_state_t;

static struct {
	dr_t	lbrake, rbrake;
	dr_t	pbrake;
	dr_t	rot_force_N;
	dr_t	axial_force;
	dr_t	local_x, local_y, local_z;
	dr_t	hdg;
	dr_t	vx, vz;
	dr_t	sim_time;
	dr_t	acf_mass;
	dr_t	tire_z;
	dr_t	nw_steerdeg1, nw_steerdeg2;
	dr_t	tire_steer_cmd;
	dr_t	override_steer;

	dr_t	camera_fov_h, camera_fov_v;
	dr_t	view_is_ext;
} drs;

static bool_t inited = B_FALSE, cam_inited = B_FALSE, started = B_FALSE;
static bp_state_t bp;

static float bp_run(void);

static int
compute_segs(const acf_t *acf, vect2_t start_pos, double start_hdg,
    vect2_t end_pos, double end_hdg, list_t *segs)
{
	seg_t *s1, *s2;
	vect2_t turn_edge, s1_v, s2_v, s2e_v;
	double rhdg, min_radius, l1, l2, x, a, r;
	bool_t backward;

	/* If the start & end positions overlap, no operation is required */
	if (VECT2_EQ(start_pos, end_pos))
		return (start_hdg == end_hdg ? 0 : -1);
	s2e_v = vect2_sub(end_pos, start_pos);
	rhdg = rel_hdg(start_hdg, dir2hdg(s2e_v));
	backward = (fabs(rhdg) > 90);

	/*
	 * If the amount of heading change is tiny, just project the desired
	 * end point onto a straight vector from our starting position and
	 * construct a single straight segment to reach that point.
	 */
	if (fabs(start_hdg - end_hdg) < 1) {
		vect2_t dir_v = hdg2dir(start_hdg + (backward ? 180 : 0));
		double len = vect2_dotprod(dir_v, s2e_v);

		end_pos = vect2_add(vect2_set_abs(dir_v, len), start_pos);

		s1 = calloc(1, sizeof (*s1));
		s1->type = SEG_TYPE_STRAIGHT;
		s1->start_pos = start_pos;
		s1->start_hdg = start_hdg;
		s1->end_pos = end_pos;
		s1->end_hdg = end_hdg;
		s1->backward = backward;
		s1->len = len;
		DEBUG_PRINT_SEG(bp, 1, s1);

		list_insert_tail(segs, s1);

		return (1);
	}

	s1_v = vect2_set_abs(hdg2dir(start_hdg), 1e10);
	if (backward)
		s1_v = vect2_neg(s1_v);
	s2_v = vect2_set_abs(hdg2dir(end_hdg), 1e10);
	if (!backward)
		s2_v = vect2_neg(s2_v);

	turn_edge = vect2vect_isect(s1_v, start_pos, s2_v, end_pos, B_TRUE);
	if (IS_NULL_VECT(turn_edge)) {
		dbg_log(bp, 1, "Turn edge undefined");
		return (-1);
	}

	l1 = vect2_abs(vect2_sub(turn_edge, start_pos));
	l2 = vect2_abs(vect2_sub(turn_edge, end_pos));
	x = MIN(l1, l2);
	l1 -= x;
	l2 -= x;

	/*
	 * Compute minimum radius using less than max_nw_angl (hence
	 * SEG_TURN_MULT), to allow for some oversteering correction.
	 */
	min_radius = tan(DEG2RAD(acf->max_nw_angle * SEG_TURN_MULT)) *
	    acf->wheelbase;
	a = (180 - ABS(rel_hdg(start_hdg, end_hdg)));
	r = x * tan(DEG2RAD(a / 2));
	if (r < min_radius) {
		dbg_log(bp, 1, "Turn too tight: %.2f < %.2f", r, min_radius);
		return (-1);
	}

	if (l1 == 0) {
		/* No initial straight segment */
		s2 = calloc(1, sizeof (*s2));
		s2->type = SEG_TYPE_STRAIGHT;
		s2->start_pos = vect2_add(end_pos, vect2_set_abs(s2_v, l2));
		s2->start_hdg = end_hdg;
		s2->end_pos = end_pos;
		s2->end_hdg = end_hdg;
		s2->backward = backward;
		s2->len = l2;

		s1 = calloc(1, sizeof (*s1));
		s1->type = SEG_TYPE_TURN;
		s1->start_pos = start_pos;
		s1->start_hdg = start_hdg;
		s1->end_pos = s2->start_pos;
		s1->end_hdg = s2->start_hdg;
		s1->backward = backward;
		s1->turn.r = r;
		s1->turn.right = (rhdg >= 0);
	} else {
		/* No final straight segment */
		s1 = calloc(1, sizeof (*s1));
		s1->type = SEG_TYPE_STRAIGHT;
		s1->start_pos = start_pos;
		s1->start_hdg = start_hdg;
		s1->end_pos = vect2_add(start_pos, vect2_set_abs(s1_v, l1));
		s1->end_hdg = start_hdg;
		s1->backward = backward;
		s1->len = l1;

		s2 = calloc(1, sizeof (*s2));
		s2->type = SEG_TYPE_TURN;
		s2->start_pos = s1->end_pos;
		s2->start_hdg = s1->end_hdg;
		s2->end_pos = end_pos;
		s2->end_hdg = end_hdg;
		s2->backward = backward;
		s2->turn.r = r;
		s2->turn.right = (rhdg >= 0);
	}

	DEBUG_PRINT_SEG(bp, 1, s1);
	DEBUG_PRINT_SEG(bp, 1, s2);

	list_insert_tail(segs, s1);
	list_insert_tail(segs, s2);

	return (2);
}

static void
turn_nosewheel(double req_angle, double rate)
{
	double rate_of_turn, steer_incr, cur_nw_angle;

	cur_nw_angle = bp_dr_getf(&drs.tire_steer_cmd);
	if (cur_nw_angle == req_angle)
		return;

	/*
	 * Modulate the steering increment to always be
	 * correctly within our rate of turn limits.
	 */
	rate_of_turn = rate * bp.d_t;
	steer_incr = MIN(ABS(req_angle - cur_nw_angle), rate_of_turn);
	cur_nw_angle += (cur_nw_angle < req_angle ? steer_incr : -steer_incr);
	/* prevent excessive deflection */
	cur_nw_angle = MIN(cur_nw_angle, bp.acf.max_nw_angle);
	cur_nw_angle = MAX(cur_nw_angle, -bp.acf.max_nw_angle);
	bp_dr_setf(&drs.tire_steer_cmd, cur_nw_angle);
}

static void
push_at_speed(double targ_speed, double max_accel)
{
	double force_lim, force_incr, force, angle_rad, accel_now, d_v, Fx, Fz;

	/*
	 * Multiply force limit by weight in tons - that's at most how
	 * hard we'll try to push the aircraft. This prevents us from
	 * flinging the aircraft across the tarmac in case some external
	 * factor is blocking us (like chocks).
	 */
	force_lim = FORCE_PER_TON * (bp.acf.mass / 1000);
	/*
	 * The maximum single-second force increment is 1/10 of the maximum
	 * pushback force limit. This means it'll take up to 10s for us to
	 * apply full pushback force.
	 */
	force_incr = (force_lim / 10) * bp.d_t;

	force = bp.last_force;
	accel_now = (bp.cur_spd - bp.last_spd) / bp.d_t;
	d_v = targ_speed - bp.cur_spd;

	/*
	 * Calculate the vector components of our force on the aircraft
	 * to correctly apply angular momentum forces below.
	 * N.B. we only push in the horizontal plane, hence no Fy component.
	 */
	angle_rad = DEG2RAD(bp_dr_getf(&drs.tire_steer_cmd));
	Fx = -force * sin(angle_rad);
	Fz = force * cos(angle_rad);

	bp_dr_setf(&drs.axial_force, bp_dr_getf(&drs.axial_force) + Fz);
	bp_dr_setf(&drs.rot_force_N, bp_dr_getf(&drs.rot_force_N) -
	    Fx * bp.acf.nw_z);

	/*
	 * This is some fudge needed to get some high-thrust aircraft
	 * going, otherwise we'll just jitter in-place due to thinking
	 * we're overdoing acceleration.
	 */
	if (ABS(bp.cur_spd) < BREAKAWAY_THRESH)
		max_accel *= 100;

	if (d_v > 0) {
		if (d_v < max_accel && ABS(bp.cur_spd) >= BREAKAWAY_THRESH)
			max_accel = d_v;
		if (accel_now > max_accel)
			force += force_incr;
		else if (accel_now < max_accel)
			force -= force_incr;
	} else if (d_v < 0) {
		max_accel *= -1;
		if (d_v > max_accel && ABS(bp.cur_spd) >= BREAKAWAY_THRESH)
			max_accel = d_v;
		if (accel_now < max_accel)
			force -= force_incr;
		else if (accel_now > max_accel)
			force += force_incr;
	}

	/* Don't overstep the force limits for this aircraft */
	force = MIN(force_lim, force);
	force = MAX(-force_lim, force);

	bp.last_force = force;
	dbg_log(bp, 1, "cur_spd: %.2f  targ: %.2f  maccel: %.2f  "
	    "accel_now: %.2f  d_v: %.2f force: %.3f",
	    bp.cur_spd, targ_speed, max_accel, accel_now, d_v, force);
}

static void
straight_run(vect2_t s, double hdg, double speed)
{
	vect2_t c, c2s, align_s, dir_v;
	double c2s_hdg, mis_hdg;

	/*
	 * Here we implement trying to keep the aircraft stable in crosswinds.
	 * We deflect the nosewheel, trying to keep an imaginary point `c'
	 * along the aircraft's centerline axis and displaced from its origin
	 * point one wheelbase back (or forward, if towing) on a straight line
	 * from the start of the straight run along the run heading.
	 * Nosewheel deflection is calculated using two parameters:
	 * 1) displacement of the point from the line. The further this point
	 *    is displaced, the further we counter-steer to get it back.
	 * 2) rate of aircraft heading change (hdg_rate). We use this to dampen
	 *    the step above, so we don't yo-yo through the center point.
	 * Steering commands are given as increments to the currently commanded
	 * nosewheel deflection, rather than the absolute value. This allows us
	 * to settle into a deflected state once alignment is achieved to help
	 * continuously counter a constant crosswind.
	 */

	/* this is the point we're tring to align */
	c = vect2_add(bp.cur_pos, vect2_set_abs(hdg2dir(bp.cur_hdg),
	    2 * (speed > 0 ? bp.acf.wheelbase : -bp.acf.wheelbase)));

	/* We project our position onto the ideal straight line. */
	dir_v = hdg2dir(hdg);
	align_s = vect2_add(s, vect2_scmul(dir_v, vect2_dotprod(vect2_sub(
	    bp.cur_pos, s), dir_v)));

	/*
	 * Calculate a direction vector pointing from s to c (or
	 * vice versa if pushing back) and transform into a heading.
	 */
	c2s = vect2_sub(align_s, c);
	c2s_hdg = dir2hdg(c2s);

	/*
	 * Calculate the required steering change. mis_hdg is the angle by
	 * which point `c' is deflected from the ideal straight line. So
	 * simply steer in the opposite direction to try and nullify it.
	 */
	mis_hdg = rel_hdg(hdg, c2s_hdg);

	/* Steering works in reverse when pushing back. */
	if (speed > 0)
		mis_hdg = -mis_hdg;

	dbg_log(bp, 1, "mis_hdg: %.1f hdg:%.1f c2s_hdg: %.1f",
	    mis_hdg, hdg, c2s_hdg);

	turn_nosewheel(mis_hdg, STRAIGHT_STEER_RATE);
	push_at_speed(speed, STRAIGHT_ACCEL);
}

static void
turn_run(vect2_t c, double radius, bool_t right, bool_t backward)
{
	vect2_t refpt = vect2_add(bp.cur_pos,
	    vect2_set_abs(vect2_neg(hdg2dir(bp.cur_hdg)), bp.acf.main_z));
	double act_radius = vect2_abs(vect2_sub(refpt, c));
	double d_radius = act_radius - radius;
	vect2_t c2r, p1, p2, r, p1_to_r, p2_to_r;
	double mis_hdg, speed;

	/* Don't turn the nosewheel if we're traveling in the wrong direction */
	if ((!backward && bp.cur_spd < 0) || (backward && bp.cur_spd > 0) ||
	    /* We're inside the turn radius, straighten out to catch up */
	    (d_radius < 0)) {
		turn_nosewheel(0, TURN_STEER_RATE);
		push_at_speed(STRAIGHT_SPEED * (backward ? -1 : 1), TURN_ACCEL);
		return;
	}

	c2r = vect2_set_abs(vect2_sub(bp.cur_pos, c), radius);
	r = vect2_add(c, c2r);
	if (!backward)
		p1 = vect2_add(r, vect2_norm(c2r, right));
	else
		p1 = vect2_add(r, vect2_norm(c2r, !right));

	p2 = vect2_add(bp.cur_pos, vect2_set_abs(hdg2dir(bp.cur_hdg),
	    2 * (!backward ? bp.acf.wheelbase : -bp.acf.wheelbase)));

	p1_to_r = vect2_sub(r, p1);
	p2_to_r = vect2_sub(r, p2);
	mis_hdg = rel_hdg(dir2hdg(p2_to_r), dir2hdg(p1_to_r));
	/* Steering works in reverse when pushing back. */
	if (backward)
		mis_hdg = -mis_hdg;

	/*
	 * Control as a function of how much turn correction we need to
	 * apply. Near maximum turn deflection slow down to TURN_SPEED,
	 * otherwise go near maximum STRAIGHT_SPEED.
	 */
	speed = (backward ? -1 : 1) * (TURN_SPEED +
	    (STRAIGHT_SPEED - TURN_SPEED) *
	    (ABS(mis_hdg) / bp.acf.max_nw_angle));

	dbg_log(bp, 1, "mis_hdg: %.1f speed: %.2f", mis_hdg, speed);

	turn_nosewheel(mis_hdg, TURN_STEER_RATE);
	push_at_speed(speed, TURN_ACCEL);
}

bool_t
bp_init(void)
{
	double tire_z_main[8];
	int n_main;

	if (inited)
		return (B_TRUE);

	memset(&drs, 0, sizeof (drs));

	bp_dr_init(&drs.lbrake, "sim/cockpit2/controls/left_brake_ratio");
	bp_dr_init(&drs.rbrake, "sim/cockpit2/controls/right_brake_ratio");
	bp_dr_init(&drs.pbrake, "sim/flightmodel/controls/parkbrake");
	bp_dr_init(&drs.rot_force_N, "sim/flightmodel/forces/N_plug_acf");
	bp_dr_init(&drs.axial_force, "sim/flightmodel/forces/faxil_plug_acf");
	bp_dr_init(&drs.local_x, "sim/flightmodel/position/local_x");
	bp_dr_init(&drs.local_y, "sim/flightmodel/position/local_y");
	bp_dr_init(&drs.local_z, "sim/flightmodel/position/local_z");
	bp_dr_init(&drs.hdg, "sim/flightmodel/position/psi");
	bp_dr_init(&drs.vx, "sim/flightmodel/position/local_vx");
	bp_dr_init(&drs.vz, "sim/flightmodel/position/local_vz");
	bp_dr_init(&drs.sim_time, "sim/time/total_running_time_sec");
	bp_dr_init(&drs.acf_mass, "sim/flightmodel/weight/m_total");
	bp_dr_init(&drs.tire_z, "sim/flightmodel/parts/tire_z_no_deflection");
	bp_dr_init(&drs.nw_steerdeg1, "sim/aircraft/gear/acf_nw_steerdeg1");
	bp_dr_init(&drs.nw_steerdeg2, "sim/aircraft/gear/acf_nw_steerdeg2");
	bp_dr_init(&drs.tire_steer_cmd,
	    "sim/flightmodel/parts/tire_steer_cmd");
	bp_dr_init(&drs.override_steer,
	    "sim/operation/override/override_wheel_steer");

	bp_dr_init(&drs.camera_fov_h,
	    "sim/graphics/view/field_of_view_deg");
	bp_dr_init(&drs.camera_fov_v,
	    "sim/graphics/view/vertical_field_of_view_deg");
	bp_dr_init(&drs.view_is_ext, "sim/graphics/view/view_is_external");

	memset(&bp, 0, sizeof (bp));
	list_create(&bp.segs, sizeof (seg_t), offsetof(seg_t, node));

	bp.turn_c_pos = NULL_VECT2;

	bp.acf.mass = bp_dr_getf(&drs.acf_mass);
	bp_dr_getvf(&drs.tire_z, &bp.acf.nw_z, 0, 1);
	n_main = bp_dr_getvf(&drs.tire_z, tire_z_main, 1, 8);
	if (n_main < 1) {
		/* Huh? Aircraft only has one gear leg?! */
		logMsg("Aircraft only has one gear leg?!");
		return (B_FALSE);
	}
	for (int i = 0; i < n_main; i++)
		bp.acf.main_z += tire_z_main[i];
	bp.acf.main_z /= n_main;
	bp.acf.wheelbase = bp.acf.main_z - bp.acf.nw_z;
	if (bp.acf.wheelbase <= 0) {
		logMsg("Aircraft has non-positive wheelbase. "
		    "Sorry, tail draggers aren't supported.");
		return (B_FALSE);
	}
	bp.acf.max_nw_angle = MAX(bp_dr_getf(&drs.nw_steerdeg1),
	    bp_dr_getf(&drs.nw_steerdeg2));

	dbg_log(bp, 1, "mass: %.0f nw_z: %.1f main_z: %.1f wheelbase: %.1f "
	    "nw_max: %.1f", bp.acf.mass, bp.acf.nw_z, bp.acf.main_z,
	    bp.acf.wheelbase, bp.acf.max_nw_angle);

	inited = B_TRUE;

	return (B_TRUE);
}

void
bp_start(void)
{
	if (started)
		return;

	if (list_head(&bp.segs) == NULL) {
		XPLMSpeakString("Please first start the pushback camera to "
		    "tell me where you want to go.");
		return;
	}

	XPLMRegisterFlightLoopCallback((XPLMFlightLoop_f)bp_run, -1, NULL);
	started = B_TRUE;
}

void
bp_stop(void)
{
	if (!started)
		return;
	/* Delete all pushback segments, that'll stop us */
	for (seg_t *seg = list_head(&bp.segs); seg != NULL;
	    seg = list_head(&bp.segs)) {
		list_remove_head(&bp.segs);
		free(seg);
	}
}

void
bp_fini(void)
{
	if (!inited)
		return;

	bp_dr_seti(&drs.override_steer, 0);

	if (started) {
		XPLMUnregisterFlightLoopCallback((XPLMFlightLoop_f)bp_run,
		    NULL);
		started = B_FALSE;
	}

	for (seg_t *seg = list_head(&bp.segs); seg != NULL;
	    seg = list_head(&bp.segs)) {
		list_remove_head(&bp.segs);
		free(seg);
	}
	list_destroy(&bp.segs);

	inited = B_FALSE;
}

void
bp_gather(void)
{
	/*
	 * CAREFUL!
	 * X-Plane's north-south axis (Z) is flipped to our understanding, so
	 * whenever we access 'local_z' or 'vz', we need to flip it.
	 */
	bp.cur_pos = VECT2(bp_dr_getf(&drs.local_x),
	    -bp_dr_getf(&drs.local_z));
	bp.cur_hdg = bp_dr_getf(&drs.hdg);
	bp.cur_t = bp_dr_getf(&drs.sim_time);
	bp.cur_spd = vect2_dotprod(hdg2dir(bp.cur_hdg),
	    VECT2(bp_dr_getf(&drs.vx), -bp_dr_getf(&drs.vz)));
}

static float
bp_run(void)
{
	seg_t *seg;
	bool_t last = B_FALSE;

	bp_gather();

	if (bp.cur_t <= bp.last_t)
		return (B_TRUE);

	bp.d_pos = vect2_sub(bp.cur_pos, bp.last_pos);
	bp.d_hdg = bp.cur_hdg - bp.last_hdg;
	bp.d_t = bp.cur_t - bp.last_t;

	bp_dr_seti(&drs.override_steer, 1);

	while ((seg = list_head(&bp.segs)) != NULL) {
		last = B_TRUE;
		/* Pilot pressed brake pedals or set parking brake, stop */
		if (bp_dr_getf(&drs.lbrake) > BRAKE_PEDAL_THRESH ||
		    bp_dr_getf(&drs.rbrake) > BRAKE_PEDAL_THRESH ||
		    bp_dr_getf(&drs.pbrake) != 0) {
			dbg_log(bp, 2, "Brakes ON, STOPPING! (%.3f/%.3f/%f)",
			    bp_dr_getf(&drs.lbrake), bp_dr_getf(&drs.rbrake),
			    bp_dr_getf(&drs.pbrake));
			break;
		}
		if (seg->type == SEG_TYPE_STRAIGHT) {
			double len = vect2_abs(vect2_sub(bp.cur_pos,
			    seg->start_pos));
			if (len >= seg->len) {
				list_remove(&bp.segs, seg);
				free(seg);
				continue;
			}
			if (!seg->backward) {
				straight_run(seg->start_pos,
				    normalize_hdg(-seg->start_hdg),
				    STRAIGHT_SPEED);
			} else {
				straight_run(seg->start_pos,
				    seg->start_hdg, -STRAIGHT_SPEED);
			}
		} else {
			vect2_t c;
			if (fabs(rel_hdg(bp.cur_hdg, seg->end_hdg)) <
			    TURN_COMPLETE_THRESH) {
				list_remove(&bp.segs, seg);
				free(seg);
				continue;
			}
			/*
			 * `c' is the center of the turn. Displace it
			 * at right angle to start_hdg at start_pos by
			 * the turn radius.
			 */
			c = vect2_add(vect2_set_abs(vect2_norm(hdg2dir(
			    seg->start_hdg), seg->turn.right), seg->turn.r),
			    seg->start_pos);
			turn_run(c, seg->turn.r, seg->turn.right,
			    seg->backward);
		}
		break;
	}

	bp.last_pos = bp.cur_pos;
	bp.last_hdg = bp.cur_hdg;
	bp.last_t = bp.cur_t;
	bp.last_spd = bp.cur_spd;

	if (seg != NULL) {
		return (-1);
	} else {
		if (last)
			bp.stopping = B_TRUE;
		turn_nosewheel(0, STRAIGHT_STEER_RATE);
		push_at_speed(0, STRAIGHT_ACCEL);
		if (ABS(bp.cur_spd) < SPEED_COMPLETE_THRESH && !bp.stopped) {
			XPLMSpeakString("Operation complete, set parking "
			    "brake");
			bp.stopped = B_TRUE;
		}
		if (bp_dr_getf(&drs.pbrake) == 0) {
			return (-1);
		}
		bp_dr_seti(&drs.override_steer, 0);
		started = B_FALSE;
		XPLMSpeakString("Disconnected, have a nice day");
		return (0);
	}
}

static vect3_t cam_pos;
static double cam_height;
static double cam_hdg;
static double cursor_hdg;
static XPLMObjectRef prediction_obj = NULL;
static list_t pred_segs;
static XPLMCommandRef circle_view_cmd;
static XPLMWindowID fake_win;
static vect2_t cursor_world_pos;
static bool_t force_root_win_focus = B_TRUE;

#define	ABV_TERR_HEIGHT		1.5	/* meters */
#define	MAX_PRED_DISTANCE	10000	/* meters */
#define	ANGLE_DRAW_STEP		5
#define	ORIENTATION_LINE_LEN	200

#define	INCR_SMALL		1
#define	INCR_MED		2
#define	INCR_BIG		4

typedef struct {
	const char	*name;
	XPLMCommandRef	cmd;
	vect3_t		incr;
} view_cmd_info_t;

static view_cmd_info_t view_cmds[] = {
    { .name = "sim/general/left", .incr = VECT3(-INCR_MED, 0, 0) },
    { .name = "sim/general/right", .incr = VECT3(INCR_MED, 0, 0) },
    { .name  = "sim/general/up", .incr = VECT3(0, 0, INCR_MED) },
    { .name = "sim/general/down", .incr = VECT3(0, 0, -INCR_MED) },
    { .name = "sim/general/forward", .incr = VECT3(0, -INCR_MED, 0) },
    { .name = "sim/general/backward", .incr = VECT3(0, INCR_MED, 0) },
    { .name = "sim/general/zoom_in", .incr = VECT3(0, -INCR_MED, 0) },
    { .name = "sim/general/zoom_out", .incr = VECT3(0, INCR_MED, 0) },
    { .name = "sim/general/hat_switch_left", .incr = VECT3(-INCR_MED, 0, 0) },
    { .name = "sim/general/hat_switch_right", .incr = VECT3(INCR_MED, 0, 0) },
    { .name = "sim/general/hat_switch_up", .incr = VECT3(0, 0, INCR_MED) },
    { .name = "sim/general/hat_switch_down", .incr = VECT3(0, 0, -INCR_MED) },
    { .name = "sim/general/hat_switch_up_left",
	.incr = VECT3(-INCR_MED, 0, INCR_MED) },
    { .name = "sim/general/hat_switch_up_right",
	.incr = VECT3(INCR_MED, 0, INCR_MED) },
    { .name = "sim/general/hat_switch_down_left",
	.incr = VECT3(-INCR_MED, 0, -INCR_MED) },
    { .name = "sim/general/hat_switch_down_right",
	.incr = VECT3(INCR_MED, 0, -INCR_MED) },
    { .name = "sim/general/left_fast", .incr = VECT3(-INCR_BIG, 0, 0) },
    { .name = "sim/general/right_fast", .incr = VECT3(INCR_BIG, 0, 0) },
    { .name = "sim/general/up_fast", .incr = VECT3(0, 0, INCR_BIG) },
    { .name = "sim/general/down_fast", .incr = VECT3(0, 0, -INCR_BIG) },
    { .name = "sim/general/forward_fast", .incr = VECT3(0, -INCR_BIG, 0) },
    { .name = "sim/general/backward_fast", .incr = VECT3(0, INCR_BIG, 0) },
    { .name = "sim/general/zoom_in_fast", .incr = VECT3(0, -INCR_BIG, 0) },
    { .name = "sim/general/zoom_out_fast", .incr = VECT3(0, INCR_BIG, 0) },
    { .name = "sim/general/left_slow", .incr = VECT3(-INCR_SMALL, 0, 0) },
    { .name = "sim/general/right_slow", .incr = VECT3(INCR_SMALL, 0, 0) },
    { .name = "sim/general/up_slow", .incr = VECT3(0, 0, INCR_SMALL) },
    { .name = "sim/general/down_slow", .incr = VECT3(0, 0, -INCR_SMALL) },
    { .name = "sim/general/forward_slow", .incr = VECT3(0, -INCR_SMALL, 0) },
    { .name = "sim/general/backward_slow", .incr = VECT3(0, INCR_SMALL, 0) },
    { .name = "sim/general/zoom_in_slow", .incr = VECT3(0, -INCR_SMALL, 0) },
    { .name = "sim/general/zoom_out_slow", .incr = VECT3(0, INCR_SMALL, 0) },
    { .name = NULL }
};

static int
move_camera(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
	unsigned i = (uintptr_t)refcon;
	vect2_t v = vect2_rot(VECT2(view_cmds[i].incr.x, view_cmds[i].incr.z),
	    cam_hdg);
	UNUSED(cmd);
	UNUSED(phase);
	cam_pos = vect3_add(cam_pos, VECT3(v.x, view_cmds[i].incr.y, v.y));
	return (0);
}

static int
cam_ctl(XPLMCameraPosition_t *pos, int losing_control, void *refcon)
{
	int x, y, w, h;
	double rx, ry, dx, dy, rw, rh;
	double fov_h, fov_v;
	vect2_t start_pos, end_pos;
	double start_hdg;
	seg_t *seg;
	int n;

	UNUSED(refcon);

	if (pos == NULL || losing_control || !cam_inited)
		return (0);

	pos->x = cam_pos.x;
	pos->y = cam_pos.y + cam_height;
	pos->z = -cam_pos.z;
	pos->pitch = -90;
	pos->heading = cam_hdg;
	pos->roll = 0;
	pos->zoom = 1;

	XPLMGetMouseLocation(&x, &y);
	XPLMGetScreenSize(&w, &h);
	/* make the mouse coordinates relative to the screen center */
	rx = ((double)x - w / 2) / (w / 2);
	ry = ((double)y - h / 2) / (h / 2);
	fov_h = DEG2RAD(bp_dr_getf(&drs.camera_fov_h));
	fov_v = DEG2RAD(bp_dr_getf(&drs.camera_fov_v));
	rw = cam_height * tan(fov_h / 2);
	rh = cam_height * tan(fov_v / 2);
	dx = rw * rx;
	dy = rh * ry;

	/*
	 * Don't make predictions if due to the camera FOV angle (>= 180 deg)
	 * we could be placing the prediction object very far away.
	 */
	if (dx > MAX_PRED_DISTANCE || dy > MAX_PRED_DISTANCE)
		return (1);

	for (seg = list_head(&pred_segs); seg != NULL;
	    seg = list_head(&pred_segs)) {
		list_remove(&pred_segs, seg);
		free(seg);
	}

	seg = list_tail(&bp.segs);
	if (seg != NULL) {
		start_pos = seg->end_pos;
		start_hdg = seg->end_hdg;
	} else {
		start_pos = VECT2(bp_dr_getf(&drs.local_x),
		    -bp_dr_getf(&drs.local_z));	/* inverted X-Plane Z */
		start_hdg = bp_dr_getf(&drs.hdg);
	}

	end_pos = vect2_add(VECT2(cam_pos.x, cam_pos.z),
	    vect2_rot(VECT2(dx, dy), pos->heading));
	cursor_world_pos = VECT2(end_pos.x, end_pos.y);

	n = compute_segs(&bp.acf, start_pos, start_hdg, end_pos, cursor_hdg,
	    &pred_segs);
	if (n > 0) {
		seg = list_tail(&pred_segs);
		seg->user_placed = B_TRUE;
	}

	return (1);
}

static void
draw_segment(const seg_t *seg)
{
	XPLMProbeRef probe = XPLMCreateProbe(xplm_ProbeY);
	XPLMProbeInfo_t info = { .structSize = sizeof (XPLMProbeInfo_t) };

	glColor4f(0, 0, 1, 1);
	glLineWidth(2);
	glPointSize(4);

	switch (seg->type) {
	case SEG_TYPE_STRAIGHT:
		glBegin(GL_LINES);
		VERIFY3U(XPLMProbeTerrainXYZ(probe, seg->start_pos.x, 0,
		    -seg->start_pos.y, &info), ==, xplm_ProbeHitTerrain);
		glVertex3f(seg->start_pos.x, info.locationY + ABV_TERR_HEIGHT,
		    -seg->start_pos.y);
		VERIFY3U(XPLMProbeTerrainXYZ(probe, seg->end_pos.x, 0,
		    -seg->end_pos.y, &info), ==, xplm_ProbeHitTerrain);
		glVertex3f(seg->end_pos.x, info.locationY + ABV_TERR_HEIGHT,
		    -seg->end_pos.y);
		glEnd();
		break;
	case SEG_TYPE_TURN: {
		vect2_t c = vect2_add(seg->start_pos, vect2_scmul(
		    vect2_norm(hdg2dir(seg->start_hdg), seg->turn.right),
		    seg->turn.r));
		vect2_t c2s = vect2_sub(seg->start_pos, c);
		double s, e, rhdg;

		glBegin(GL_LINES);
		rhdg = rel_hdg(seg->start_hdg, seg->end_hdg);
		s = MIN(0, rhdg);
		e = MAX(0, rhdg);
		ASSERT3F(s, <=, e);
		for (double a = s; a < e; a += ANGLE_DRAW_STEP) {
			vect2_t p;
			double s = MIN(ANGLE_DRAW_STEP, e - a);
			p = vect2_add(c, vect2_rot(c2s, a));
			VERIFY3U(XPLMProbeTerrainXYZ(probe, p.x, 0, -p.y,
			    &info), ==, xplm_ProbeHitTerrain);
			glVertex3f(p.x, info.locationY + ABV_TERR_HEIGHT, -p.y);
			p = vect2_add(c, vect2_rot(c2s, a + s));
			glVertex3f(p.x, info.locationY + ABV_TERR_HEIGHT, -p.y);
		}
		glEnd();
		break;
	}
	}

	XPLMDestroyProbe(probe);
}

static int
draw_prediction(XPLMDrawingPhase phase, int before, void *refcon)
{
	XPLMDrawInfo_t loc;
	seg_t *seg;
	XPLMProbeRef probe = XPLMCreateProbe(xplm_ProbeY);
	XPLMProbeInfo_t info = { .structSize = sizeof (XPLMProbeInfo_t) };

	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);

	if (bp_dr_geti(&drs.view_is_ext) != 1)
		XPLMCommandOnce(circle_view_cmd);

	for (seg = list_head(&bp.segs); seg != NULL;
	    seg = list_next(&bp.segs, seg))
		draw_segment(seg);

	for (seg = list_head(&pred_segs); seg != NULL;
	    seg = list_next(&pred_segs, seg))
		draw_segment(seg);

	if ((seg = list_tail(&pred_segs)) != NULL) {
		vect2_t dir_v = hdg2dir(seg->end_hdg);
		vect2_t x;

		VERIFY3U(XPLMProbeTerrainXYZ(probe, seg->end_pos.x, 0,
		    -seg->end_pos.y, &info), ==, xplm_ProbeHitTerrain);
		loc.structSize = sizeof (XPLMDrawInfo_t);
		loc.x = seg->end_pos.x;
		loc.y = info.locationY + ABV_TERR_HEIGHT;
		loc.z = -seg->end_pos.y;	/* invert X-Plane Z */
		loc.pitch = 0;
		loc.heading = seg->end_hdg;
		loc.roll = 0;

		if (seg->type == SEG_TYPE_TURN || !seg->backward) {
			glColor4f(0, 1, 0, 1);
			glBegin(GL_LINES);
			glVertex3f(seg->end_pos.x, info.locationY +
			    ABV_TERR_HEIGHT, -seg->end_pos.y);
			x = vect2_add(seg->end_pos, vect2_scmul(dir_v,
			    ORIENTATION_LINE_LEN));
			glVertex3f(x.x, info.locationY + ABV_TERR_HEIGHT, -x.y);
			glEnd();
		}
		if (seg->type == SEG_TYPE_TURN || seg->backward) {
			glColor4f(1, 0, 0, 1);
			glBegin(GL_LINES);
			glVertex3f(seg->end_pos.x, info.locationY +
			    ABV_TERR_HEIGHT, -seg->end_pos.y);
			x = vect2_add(seg->end_pos, vect2_neg(vect2_scmul(
			    dir_v, ORIENTATION_LINE_LEN)));
			glVertex3f(x.x, info.locationY + ABV_TERR_HEIGHT, -x.y);
			glEnd();
		}

		XPLMDrawObjects(prediction_obj, 1, &loc, 0, 0);
	} else {
		vect2_t v;
		double cross_sz = cam_height / 30;
		double y;

		/*
		 * Draw a red cross under the cursor. Remember to apply
		 * inverted X-Plane Z coords to final draw calls.
		 */

		VERIFY3U(XPLMProbeTerrainXYZ(probe, cursor_world_pos.x, 0,
		    -cursor_world_pos.y, &info), ==, xplm_ProbeHitTerrain);
		y = info.locationY + ABV_TERR_HEIGHT;

		glColor4f(1, 0, 0, 1);
		glLineWidth(3);
		glBegin(GL_LINES);
		v = vect2_add(vect2_rot(VECT2(-cross_sz, cross_sz), cam_hdg),
		    cursor_world_pos);
		glVertex3f(v.x, y, -v.y);
		v = vect2_add(vect2_rot(VECT2(cross_sz, -cross_sz), cam_hdg),
		    cursor_world_pos);
		glVertex3f(v.x, y, -v.y);
		v = vect2_add(vect2_rot(VECT2(-cross_sz, -cross_sz), cam_hdg),
		    cursor_world_pos);
		glVertex3f(v.x, y, -v.y);
		v = vect2_add(vect2_rot(VECT2(cross_sz, cross_sz), cam_hdg),
		    cursor_world_pos);
		glVertex3f(v.x, y, -v.y);
		glEnd();

		loc.structSize = sizeof (XPLMDrawInfo_t);
		loc.x = cursor_world_pos.x;
		loc.y = y;
		loc.z = -cursor_world_pos.y;	/* invert X-Plane Z */
		loc.pitch = 0;
		loc.heading = cursor_hdg;
		loc.roll = 0;
		XPLMDrawObjects(prediction_obj, 1, &loc, 0, 0);
	}


	if ((seg = list_tail(&bp.segs)) != NULL) {
		VERIFY3U(XPLMProbeTerrainXYZ(probe, seg->end_pos.x, 0,
		    -seg->end_pos.y, &info), ==, xplm_ProbeHitTerrain);
		loc.structSize = sizeof (XPLMDrawInfo_t);
		loc.x = seg->end_pos.x;
		loc.y = info.locationY + ABV_TERR_HEIGHT;
		loc.z = -seg->end_pos.y;	/* invert X-Plane Z */
		loc.pitch = 0;
		loc.heading = seg->end_hdg;
		loc.roll = 0;
		XPLMDrawObjects(prediction_obj, 1, &loc, 0, 0);
	}

	XPLMDestroyProbe(probe);

	return (1);
}

static void
fake_win_draw(XPLMWindowID inWindowID, void *inRefcon)
{
	int w, h;
	UNUSED(inWindowID);
	UNUSED(inRefcon);

	XPLMGetScreenSize(&w, &h);
	XPLMSetWindowGeometry(fake_win, 0, h, w, 0);

	if (!XPLMIsWindowInFront(fake_win))
		XPLMBringWindowToFront(fake_win);
	if (force_root_win_focus)
		XPLMTakeKeyboardFocus(0);
}

static void
fake_win_key(XPLMWindowID inWindowID, char inKey, XPLMKeyFlags inFlags,
    char inVirtualKey, void *inRefcon, int losingFocus)
{
	UNUSED(inWindowID);
	UNUSED(inKey);
	UNUSED(inFlags);
	UNUSED(inVirtualKey);
	UNUSED(inRefcon);
	UNUSED(losingFocus);
}

static XPLMCursorStatus
fake_win_cursor(XPLMWindowID inWindowID, int x, int y, void *inRefcon)
{
	UNUSED(inWindowID);
	UNUSED(x);
	UNUSED(y);
	UNUSED(inRefcon);
	return (xplm_CursorDefault);
}

#define	CLICK_THRESHOLD_US	200000

static int
fake_win_click(XPLMWindowID inWindowID, int x, int y, XPLMMouseStatus inMouse,
    void *inRefcon)
{
	static uint64_t down_t = 0;
	static int down_x = 0, down_y = 0;

	UNUSED(inWindowID);
	UNUSED(inRefcon);

	if (inMouse == xplm_MouseDown) {
		down_t = microclock();
		down_x = x;
		down_y = y;
		force_root_win_focus = B_FALSE;
	} else if (inMouse == xplm_MouseDrag) {
		if (x != down_x || y != down_y) {
			int w, h;
			double fov_h, fov_v, rx, ry, rw, rh, dx, dy;
			vect2_t v;

			XPLMGetScreenSize(&w, &h);
			rx = ((double)x - down_x) / (w / 2);
			ry = ((double)y - down_y) / (h / 2);
			fov_h = DEG2RAD(bp_dr_getf(&drs.camera_fov_h));
			fov_v = DEG2RAD(bp_dr_getf(&drs.camera_fov_v));
			rw = cam_height * tan(fov_h / 2);
			rh = cam_height * tan(fov_v / 2);
			dx = rw * rx;
			dy = rh * ry;
			v = vect2_rot(VECT2(dx, dy), cam_hdg);
			cam_pos.x -= v.x;
			cam_pos.z -= v.y;
			down_x = x;
			down_y = y;
		}
	} else if (inMouse == xplm_MouseUp &&
	    microclock() - down_t < CLICK_THRESHOLD_US) {
		/*
		 * Transfer whatever is in pred_segs to the normal segments
		 * and clear pred_segs.
		 */
		for (seg_t *seg = list_head(&pred_segs); seg != NULL;
		    seg = list_head(&pred_segs)) {
			list_remove(&pred_segs, seg);
			list_insert_tail(&bp.segs, seg);
		}
		force_root_win_focus = B_TRUE;
	}

	return (1);
}

static int
fake_win_wheel(XPLMWindowID inWindowID, int x, int y, int wheel, int clicks,
    void *inRefcon)
{
	UNUSED(inWindowID);
	UNUSED(x);
	UNUSED(y);
	UNUSED(wheel);
	UNUSED(clicks);
	UNUSED(inRefcon);

	if (wheel == 0) {
		cursor_hdg = normalize_hdg(cursor_hdg + clicks * 2);
	}

	return (0);
}

static int
key_sniffer(char inChar, XPLMKeyFlags inFlags, char inVirtualKey, void *refcon)
{
	UNUSED(inChar);
	UNUSED(refcon);

	/* Only allow the plain key to be pressed, no modifiers */
	if (inFlags != xplm_DownFlag)
		return (1);

	switch (inVirtualKey) {
	case XPLM_VK_RETURN:
	case XPLM_VK_ESCAPE:
		bp_cam_fini();
		return (0);
	case XPLM_VK_CLEAR:
	case XPLM_VK_BACK:
	case XPLM_VK_DELETE:
		/* Delete the segments up to the next user-placed segment */
		if (list_tail(&bp.segs) == NULL)
			return (0);
		list_remove_tail(&bp.segs);
		for (seg_t *seg = list_tail(&bp.segs); seg != NULL &&
		    !seg->user_placed; seg = list_tail(&bp.segs)) {
			list_remove_tail(&bp.segs);
			free(seg);
		}
		return (0);
	}

	return (1);
}

void
bp_cam_init(void)
{
	XPLMCreateWindow_t fake_win_ops = {
	    .structSize = sizeof (XPLMCreateWindow_t),
	    .left = 0, .top = 0, .right = 0, .bottom = 0, .visible = 1,
	    .drawWindowFunc = fake_win_draw,
	    .handleMouseClickFunc = fake_win_click,
	    .handleKeyFunc = fake_win_key,
	    .handleCursorFunc = fake_win_cursor,
	    .handleMouseWheelFunc = fake_win_wheel,
	    .refcon = NULL
	};

	if (cam_inited || !bp_init())
		return;

	XPLMGetScreenSize(&fake_win_ops.right, &fake_win_ops.top);

	circle_view_cmd = XPLMFindCommand("sim/view/circle");
	ASSERT(circle_view_cmd != NULL);
	XPLMCommandOnce(circle_view_cmd);

	fake_win = XPLMCreateWindowEx(&fake_win_ops);
	ASSERT(fake_win != NULL);
	XPLMBringWindowToFront(fake_win);
	XPLMTakeKeyboardFocus(fake_win);

	list_create(&pred_segs, sizeof (seg_t), offsetof(seg_t, node));
	cam_height = 100 * bp.acf.wheelbase;
	/* We keep the camera position in our coordinates for ease of manip */
	cam_pos = VECT3(bp_dr_getf(&drs.local_x),
	    bp_dr_getf(&drs.local_y), -bp_dr_getf(&drs.local_z));
	cam_hdg = bp_dr_getf(&drs.hdg);
	cursor_hdg = bp_dr_getf(&drs.hdg);
	XPLMControlCamera(xplm_ControlCameraForever, cam_ctl, NULL);

	XPLMRegisterDrawCallback(draw_prediction, xplm_Phase_Objects, 0, NULL);
	prediction_obj = XPLMLoadObject("Resources/default scenery/"
	    "airport scenery/Aircraft/General_Aviation/Cessna_172.obj");
	VERIFY(prediction_obj != NULL);

	for (int i = 0; view_cmds[i].name != NULL; i++) {
		view_cmds[i].cmd = XPLMFindCommand(view_cmds[i].name);
		VERIFY(view_cmds[i].cmd != NULL);
		XPLMRegisterCommandHandler(view_cmds[i].cmd, move_camera,
		    1, (void *)(uintptr_t)i);
	}
	XPLMRegisterKeySniffer(key_sniffer, 1, NULL);

	cam_inited = B_TRUE;
}

void
bp_cam_fini(void)
{
	if (!cam_inited)
		return;

	for (seg_t *seg = list_head(&pred_segs); seg != NULL;
	    seg = list_head(&pred_segs)) {
		list_remove(&pred_segs, seg);
		free(seg);
	}
	list_destroy(&pred_segs);
	XPLMUnregisterDrawCallback(draw_prediction, xplm_Phase_Objects,
	    0, NULL);
	XPLMUnloadObject(prediction_obj);
	XPLMDestroyWindow(fake_win);

	for (int i = 0; view_cmds[i].name != NULL; i++) {
		XPLMUnregisterCommandHandler(view_cmds[i].cmd, move_camera,
		    1, (void *)(uintptr_t)i);
	}
	XPLMUnregisterKeySniffer(key_sniffer, 1, NULL);

	cam_inited = B_FALSE;
}