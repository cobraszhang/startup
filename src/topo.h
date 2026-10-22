/* topo.h - dependency-ordered startup sequence (Kahn topological sort)
 * with startup-class partitioning (see cfg.h KIND_SYSTEM / KIND_APP).
 */
#ifndef TOPO_H
#define TOPO_H

#include "cfg.h"

/* whole model, two phases: every system-class app before every
 * application-class app, dependency order inside each phase */
#define TOPO_ALL (-1)

/* Compute a startup order honouring "deps" edges plus the class rule
 * "user applications must never start before system services".
 *
 * kind: TOPO_ALL   - both phases (system phase first, then application)
 *       KIND_SYSTEM - only system-class entries
 *       KIND_APP    - only application-class entries
 * Edges that cross from a later phase to an earlier one (application
 * depends on system) carry no constraint inside the phase - the
 * dependency is satisfied by ordering. The reverse (a system-class app
 * depending on an application-class one) is rejected: it could never
 * satisfy the class rule.
 *
 * On success returns 0; *order_out receives a malloc'ed array of
 * *cnt_out indices into apps.
 *
 * Return codes:
 *   0  ok
 *  -1  an app depends on an unknown name
 *  -2  dependency cycle detected inside a phase (apps are logged)
 *  -3  out of memory
 *  -4  system-class app depends on an application-class app
 */
int topo_sort(const app_t *apps, int n, int kind,
              int **order_out, int *cnt_out);

#endif /* TOPO_H */
