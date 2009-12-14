/* Playout player based on probability distribution generated over
 * the available moves. */

/* We use the ELO-based (Coulom, 2007) approach, where each board
 * feature (matched pattern, self-atari, capture, MC owner?, ...)
 * is pre-assigned "playing strength" (gamma).
 *
 * Then, the problem of choosing a move is basically a team
 * competition in ELO terms - each spot is represented by a team
 * of features appearing there; the team gamma is product of feature
 * gammas. The team gammas make for a probability distribution of
 * moves to be played.
 *
 * We use the general pattern classifier that will find the features
 * for us, and external datasets that can be harvested from a set
 * of game records (see the HACKING file for details): patterns.spat
 * as a dictionary of spatial stone configurations, and patterns.gamma
 * with strengths of particular features. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define DEBUG
#include "board.h"
#include "debug.h"
#include "pattern.h"
#include "playout.h"
#include "playout/elo.h"
#include "random.h"
#include "tactics.h"
#include "uct/prior.h"

#define PLDEBUGL(n) DEBUGL_(p->debug_level, n)


/* Note that the context can be shared by multiple threads! */

struct elo_policy {
	float selfatari;
	struct pattern_config pc;
	struct features_gamma *fg;
};


/* This is the core of the policy - initializes and constructs the
 * probability distribution over the move candidates. */

void
elo_get_probdist(struct playout_policy *p, struct board *b, enum stone to_play, struct probdist *pd)
{
	struct elo_policy *pp = p->data;

	probdist_init(pd, board_size2(b));

	/* First, assign per-point probabilities. */

	for (int f = 0; f < b->flen; f++) {
		struct move m = { .coord = b->f[f], .color = to_play };

		/* Skip pass (for now)? */
		if (is_pass(m.coord))
			continue;
		//fprintf(stderr, "<%d> %s\n", f, coord2sstr(m.coord, b));

		/* Skip invalid moves. */
		if (!board_is_valid_move(b, &m))
			continue;

		/* We shall never fill our own single-point eyes. */
		/* XXX: In some rare situations, this prunes the best move:
		 * Bulk-five nakade with eye at 1-1 point. */
		if (board_is_one_point_eye(b, &m.coord, to_play)) {
			continue;
		}

		/* Each valid move starts with gamma 1. */
		probdist_add(pd, m.coord, 1.f);

		/* Some easy features: */
		/* XXX: We just disable them for now since we call the
		 * pattern matcher; you need the gammas file. */
#if 0
		if (is_bad_selfatari(b, to_play, m.coord))
			probdist_mul(pd, m.coord, pp->selfatari);
#endif

		/* Match pattern features: */
		struct pattern p;
		pattern_match(&pp->pc, &p, b, &m);
		for (int i = 0; i < p.n; i++) {
			/* Multiply together gammas of all pattern features. */
			float gamma = feature_gamma(pp->fg, &p.f[i], NULL);
			probdist_mul(pd, m.coord, gamma);
		}
		//fprintf(stderr, "<%d> %s %f\n", f, coord2sstr(m.coord, b), pd->moves[m.coord]);
	}
}


coord_t
playout_elo_choose(struct playout_policy *p, struct board *b, enum stone to_play)
{
	struct probdist pd;
	elo_get_probdist(p, b, to_play, &pd);

	return probdist_pick(&pd);
}

void
playout_elo_assess(struct playout_policy *p, struct prior_map *map, int games)
{
	struct probdist pd;
	elo_get_probdist(p, map->b, map->to_play, &pd);

	/* It is a question how to transform the gamma to won games;
	 * we use a naive approach and just pass the gamma itself as
	 * value. XXX: We hope nothing breaks, since often gamma>1. */
	/* TODO: Try sqrt(p), atan(p)/pi*2. */

	for (int f = 0; f < map->b->flen; f++) {
		coord_t c = map->b->f[f];
		if (!map->consider[c])
			continue;
		add_prior_value(map, c, pd.moves[c], games);
	}

	probdist_done(&pd);
}


struct playout_policy *
playout_elo_init(char *arg)
{
	struct playout_policy *p = calloc(1, sizeof(*p));
	struct elo_policy *pp = calloc(1, sizeof(*pp));
	p->data = pp;
	p->choose = playout_elo_choose;
	p->assess = playout_elo_assess;

	/* Some defaults based on the table in Remi Coulom's paper. */
	pp->selfatari = 0.06;
	pp->pc = DEFAULT_PATTERN_CONFIG;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ":");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "selfatari") && optval) {
				pp->selfatari = atof(optval);
			} else {
				fprintf(stderr, "playout-elo: Invalid policy argument %s or missing value\n", optname);
				exit(1);
			}
		}
	}

	pp->pc.spat_dict = spatial_dict_init(false);
	pp->fg = features_gamma_init(&pp->pc);

	return p;
}