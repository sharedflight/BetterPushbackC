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

#include <acfutils/helpers.h>
#include <acfutils/math.h>

#include "driving.h"

#define	NORMAL_SPEED		1.11	/* m/s [4 km/h, "walking speed"] */
#define	FAST_SPEED		4	/* m/s [~8 knots] */
#define	CRAWL_SPEED		0.1	/* m/s */
#define	NORMAL_ACCEL		0.25	/* m/s^2 */
#define	NORMAL_DECEL		0.17	/* m/s^2 */
#define	SEG_TURN_MULT		0.9	/* leave 10% for oversteer */
#define	SPEED_COMPLETE_THRESH	0.05	/* m/s */
#define	MAX_ANG_VEL		3	/* degrees per second */
#define	MIN_TURN_RADIUS		1.5	/* in case the aircraft is tiny */
#define	MIN_STEERING_ARM_LEN	2	/* meters */
#define	HARD_STEER_ANGLE	10	/* degrees */
#define	MAX_OFF_PATH_ANGLE	35	/* degrees */
#define	STEERING_SENSITIVE	90	/* degrees */

#define	STEER_GATE(x, g)	MIN(MAX(x, -g), g)

static double turn_run_speed(list_t *segs, double rhdg, double radius,
    bool_t backward, double max_ang_vel, const seg_t *next);
static double straight_run_speed(list_t *segs, double rmng_d, bool_t backward,
    double max_ang_vel, const seg_t *next);

int
compute_segs(const vehicle_t *veh, vect2_t start_pos, double start_hdg,
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
	if (IS_NULL_VECT(turn_edge))
		return (-1);

	l1 = vect2_dist(turn_edge, start_pos);
	l2 = vect2_dist(turn_edge, end_pos);
	x = MIN(l1, l2);
	l1 -= x;
	l2 -= x;

	/*
	 * Compute minimum radius using less than max_steer (hence
	 * SEG_TURN_MULT), to allow for some oversteering correction.
	 * Also limit the radius to something sensible (MIN_TURN_RADIUS).
	 */
	min_radius = MAX(tan(DEG2RAD(90 - (veh->max_steer * SEG_TURN_MULT))) *
	    veh->wheelbase, MIN_TURN_RADIUS);
	a = (180 - ABS(rel_hdg(start_hdg, end_hdg)));
	r = x * tan(DEG2RAD(a / 2));
	if (r < min_radius)
		return (-1);
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

	list_insert_tail(segs, s1);
	list_insert_tail(segs, s2);

	return (2);
}

static void
drive_on_line(const vehicle_pos_t *pos, const vehicle_t *veh,
    vect2_t line_start, double line_hdg, double speed, double arm_len,
    double steer_corr_amp, double *last_mis_hdg, double d_t,
    double *steer_out, double *speed_out)
{
	vect2_t c, s2c, align_s, dir_v;
	double s2c_hdg, mis_hdg, steering_arm, turn_radius, ang_vel, rhdg;
	double cur_hdg, steer, d_mis_hdg;
	bool_t overcorrecting = B_FALSE;

	cur_hdg = (speed >= 0 ? pos->hdg : normalize_hdg(pos->hdg + 180));

	/* Neutralize steering until we're traveling in our direction */
	if ((speed < 0 && pos->spd > 0) || (speed > 0 && pos->spd < 0)) {
		*steer_out = 0;
		*speed_out = speed;
		return;
	}

	/* this is the point we're tring to align */
	steering_arm = MAX(arm_len, MIN_STEERING_ARM_LEN);
	c = vect2_add(pos->pos, vect2_scmul(hdg2dir(cur_hdg), steering_arm));

	/*
	 * We project our position onto the ideal straight line. Limit the
	 * projection backwards to be at least 1m ahead, otherwise we might
	 * steer in the opposite sense than we want.
	 */
	dir_v = hdg2dir(line_hdg);
	align_s = vect2_add(line_start, vect2_scmul(dir_v,
	    vect2_dotprod(vect2_sub(pos->pos, line_start), dir_v)));

	/*
	 * Calculate a direction vector pointing from s to c (or
	 * vice versa if pushing back) and transform into a heading.
	 */
	s2c = vect2_sub(c, align_s);
	s2c_hdg = dir2hdg(s2c);

	mis_hdg = rel_hdg(s2c_hdg, line_hdg);
	rhdg = rel_hdg(cur_hdg, line_hdg);
	d_mis_hdg = (mis_hdg - (*last_mis_hdg)) / d_t;

	/*
	 * Calculate the required steering change. mis_hdg is the angle by
	 * which point `c' is deflected from the ideal straight line. So
	 * simply steer in the opposite direction to try and nullify it.
	 */
	steer = STEER_GATE(mis_hdg + d_mis_hdg * steer_corr_amp,
	    veh->max_steer);

	/*
	 * Watch out for overcorrecting. If our heading is too far in the
	 * opposite direction, limit our relative angle to the desired path
	 * angle to MAX_OFF_PATH_ANGLE and steer that way until we get back
	 * on track.
	 */
	if (mis_hdg < 0 && rhdg > MAX_OFF_PATH_ANGLE) {
		steer = STEER_GATE(rhdg - MAX_OFF_PATH_ANGLE, veh->max_steer);
		overcorrecting = B_TRUE;
	} else if (mis_hdg > 0 && rhdg < -MAX_OFF_PATH_ANGLE) {
		steer = STEER_GATE(rhdg + MAX_OFF_PATH_ANGLE, veh->max_steer);
		overcorrecting = B_TRUE;
	}
	/*
	 * If we've come off the path even with overcorrection, slow down
	 * until we're re-established again.
	 */
	if (overcorrecting)
		speed = MAX(MIN(speed, NORMAL_SPEED), -NORMAL_SPEED);

	/*
	 * Limit our speed to not overstep maximum angular velocity for
	 * a correction maneuver. This helps in case we get kicked off
	 * from a straight line very far and need to correct a lot.
	 */
	turn_radius = tan(DEG2RAD(90 - ABS(steer))) * veh->wheelbase;
	ang_vel = RAD2DEG(ABS(speed) / turn_radius);
	speed *= MIN(MAX_ANG_VEL / ang_vel, 1);

	/* Steering works in reverse when pushing back. */
	if (speed < 0)
		steer = -steer;

	*steer_out = steer * steer_corr_amp;
	*speed_out = speed;

	*last_mis_hdg = mis_hdg;
}

static double
next_seg_speed(list_t *segs, const seg_t *next, bool_t cur_backward,
    double max_ang_vel)
{
	if (next != NULL && next->backward == cur_backward) {
		if (next->type == SEG_TYPE_STRAIGHT) {
			return (straight_run_speed(segs, next->len,
			    next->backward, max_ang_vel,
			    list_next(segs, next)));
		} else {
			return (turn_run_speed(segs, rel_hdg(next->start_hdg,
			    next->end_hdg), next->turn.r, next->backward,
			    max_ang_vel, list_next(segs, next)));
		}
	} else {
		/*
		 * At the end of the operation or when reversing direction,
		 * target a nearly stopped speed.
		 */
		return (CRAWL_SPEED);
	}
}

/*
 * Estimates the speed we want to achieve during a turn run. This basically
 * treats the circle we're supposed to travel as if it were a straight line
 * (thus employing the straight_run_speed algorithm), but limits the maximum
 * angular velocity around the circle to MAX_ANG_VEL (2.5 deg/s) to limit
 * side-loading. This means the tighter the turn, the slower our speed.
 */
static double
turn_run_speed(list_t *segs, double rhdg, double radius, bool_t backward,
    double max_ang_vel, const seg_t *next)
{
	double rmng_d = (2 * M_PI * radius) * (rhdg / 360.0);
	double spd = straight_run_speed(segs, rmng_d, backward, max_ang_vel,
	    next);
	double rmng_t = rmng_d / spd;
	double ang_vel = rhdg / rmng_t;

	spd *= MIN(max_ang_vel / ang_vel, 1);

	return (spd);
}

static double
straight_run_speed(list_t *segs, double rmng_d, bool_t backward,
    double max_ang_vel, const seg_t *next)
{
	double next_spd, cruise_spd, spd;
	double ts[2];

	next_spd = next_seg_speed(segs, next, backward, max_ang_vel);
	cruise_spd = (backward ? NORMAL_SPEED : FAST_SPEED);

	/*
	 * This algorithm works as follows:
	 * We know the remaining distance and the next segment's target
	 * speed. So we work backwards to determine what maximum speed
	 * we could be going in order to hit next_spd using NORMAL_DECEL.
	 *
	 *          (speed)
	 *          ^
	 * max ---> |
	 * spd      |\       (NORMAL_DECEL slope)
	 *          |  \    /
	 *          |    \ V
	 *          |      \
	 *          |        \
	 *          |          \
	 *          |           + <--- next_spd
	 *          |           |
	 *          +-----------+------------->
	 *          |   rmng_d  |    (distance)
	 *          |<--------->|
	 *
	 * Here's the general equation for acceleration:
	 *
	 * d = 1/2at^2 + vt
	 *
	 * Where:
	 *	'd' = rmng_d
	 *	'a' = NORMAL_DECEL
	 *	'v' = next_spd
	 *	't' = <unknown>
	 *
	 * This is a simple quadratic equation (1/2at^2 + vt - d = 0), so
	 * we can solve for the only unknown, time 't'. If we have two
	 * results, taking greater value i.e. the one lying in the future,
	 * we simply calculate the initial max_spd = next_spd + at. This
	 * is our theoretical maximum. Taking the lesser of that and the
	 * target cruise speed, we arrive at our final governed speed `spd'.
	 */
	switch (quadratic_solve(0.5 * NORMAL_DECEL, next_spd, -rmng_d, ts)) {
	case 1:
		spd = MIN(NORMAL_DECEL * ts[0] + next_spd, cruise_spd);
		break;
	case 2:
		spd = MIN(NORMAL_DECEL * MAX(ts[0], ts[1]) + next_spd,
		    cruise_spd);
		break;
	default:
		spd = next_spd;
		break;
	}

	return (spd);
}
static void
turn_run(const vehicle_pos_t *pos, const vehicle_t *veh, const seg_t *seg,
    double *last_mis_hdg, double d_t, double speed, double *out_steer,
    double *out_speed)
{
	double start_hdg = (!seg->backward ? seg->start_hdg :
	    normalize_hdg(seg->start_hdg + 180));
	double end_hdg = (!seg->backward ? seg->end_hdg :
	    normalize_hdg(seg->end_hdg + 180));
	vect2_t c2r, r, dir_v;
	double hdg, cur_radial, start_radial, end_radial;
	bool_t cw = ((seg->turn.right && !seg->backward) ||
	    (!seg->turn.right && seg->backward));
	/*
	 * `c' is the center of the turn. Displace it at right angle to
	 * start_hdg at start_pos by the turn radius.
	 */
	vect2_t c = vect2_add(vect2_set_abs(vect2_norm(hdg2dir(start_hdg),
	    seg->turn.right), seg->turn.r), seg->start_pos);

	c2r = vect2_set_abs(vect2_sub(pos->pos, c), seg->turn.r);
	cur_radial = dir2hdg(c2r);
	r = vect2_add(c, c2r);
	dir_v = vect2_norm(c2r, cw);
	start_radial = normalize_hdg(start_hdg + (cw ? -90 : 90));
	end_radial = normalize_hdg(end_hdg + (cw ? -90 : 90));
	if (is_on_arc(cur_radial, start_radial, end_radial, cw)) {
		hdg = dir2hdg(dir_v);
	} else if (fabs(rel_hdg(cur_radial, start_radial)) <
	    fabs(rel_hdg(cur_radial, end_radial))) {
		hdg = start_hdg;
	} else {
		hdg = end_hdg;
	}

	speed = (!seg->backward ? speed : -speed);
	drive_on_line(pos, veh, r, hdg, speed, veh->wheelbase / 5,
	    2, last_mis_hdg, d_t, out_steer, out_speed);
}

bool_t
drive_segs(const vehicle_pos_t *pos, const vehicle_t *veh, list_t *segs,
    double max_ang_vel, double *last_mis_hdg, double d_t, double *out_steer,
    double *out_speed)
{
	seg_t *seg = list_head(segs);

	ASSERT(seg != NULL);
	if (seg->type == SEG_TYPE_STRAIGHT) {
		double len = vect2_dist(pos->pos, seg->start_pos);
		double speed = straight_run_speed(segs, seg->len - len,
		    seg->backward, max_ang_vel, list_next(segs, seg));
		double hdg = (!seg->backward ? seg->start_hdg :
		    normalize_hdg(seg->start_hdg + 180));

		if (len >= seg->len) {
			list_remove(segs, seg);
			free(seg);
			return (B_FALSE);
		}

		speed = (!seg->backward ? speed : -speed);
		drive_on_line(pos, veh, seg->start_pos, hdg, speed,
		    veh->wheelbase / 2, 1.5, last_mis_hdg, d_t, out_steer,
		    out_speed);
	} else {
		double rhdg = fabs(rel_hdg(pos->hdg, seg->end_hdg));
		double end_hdg = (!seg->backward ? seg->end_hdg :
		    normalize_hdg(seg->end_hdg + 180));
		double end_brg = fabs(rel_hdg(end_hdg, dir2hdg(
		    vect2_sub(pos->pos, seg->end_pos))));
		double speed = turn_run_speed(segs, ABS(rhdg), seg->turn.r,
		    seg->backward, max_ang_vel, list_next(segs, seg));

		/*
		 * Segment complete when we are past the end_pos point
		 * (delta between end_hdg and a vector from end_pos to
		 * cur_pos is <= 90 degrees)
		 */
		if (end_brg <= 90) {
			list_remove(segs, seg);
			free(seg);
			return (B_FALSE);
		}
		turn_run(pos, veh, seg, last_mis_hdg, d_t, speed,
		    out_steer, out_speed);
	}

	return (B_TRUE);
}