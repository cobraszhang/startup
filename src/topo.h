/* topo.h - dependency-ordered startup sequence (Kahn topological sort) */
#ifndef TOPO_H
#define TOPO_H

#include "cfg.h"

/* Compute a startup order for apps[0..n-1] honouring every "deps"
 * edge. On success returns 0 and *order_out receives a malloc'ed
 * array of n indices: apps[order[0]] must be started first.
 *
 * Return codes:
 *   0  ok
 *  -1  an app depends on an unknown name
 *  -2  dependency cycle detected (offending apps are logged)
 *  -3  out of memory
 */
int topo_sort(const app_t *apps, int n, int **order_out);

#endif /* TOPO_H */
