/* topo.c - Kahn's topological sort over the dependency graph.
 *
 * Deterministic: among apps whose dependencies are all satisfied,
 * the one listed earliest in the configuration is started first.
 * Portable C89.
 */
#include <stdlib.h>

#include "topo.h"
#include "log.h"

int topo_sort(const app_t *apps, int n, int **order_out)
{
    int *indeg, *done, *order;
    int i, j, k, pick, produced = 0;

    *order_out = NULL;
    if (n <= 0) return 0;

    indeg = (int *)calloc((size_t)n, sizeof(int));
    done  = (int *)calloc((size_t)n, sizeof(int));
    order = (int *)malloc((size_t)n * sizeof(int));
    if (!indeg || !done || !order) {
        free(indeg); free(done); free(order);
        return -3;
    }

    /* validate every dependency name and compute in-degrees */
    for (i = 0; i < n; i++) {
        for (k = 0; k < apps[i].ndeps; k++) {
            j = cfg_index(apps, n, apps[i].deps[k]);
            if (j < 0) {
                log_msg("topo: app '%s' depends on unknown app '%s'",
                        apps[i].name, apps[i].deps[k]);
                free(indeg); free(done); free(order);
                return -1;
            }
            indeg[i]++;
        }
    }

    while (produced < n) {
        pick = -1;
        for (i = 0; i < n; i++) {
            if (!done[i] && indeg[i] == 0) { pick = i; break; }
        }
        if (pick < 0) {
            log_msg("topo: dependency cycle detected among:");
            for (i = 0; i < n; i++)
                if (!done[i]) log_msg("topo:   %s", apps[i].name);
            free(indeg); free(done); free(order);
            return -2;
        }
        order[produced++] = pick;
        done[pick] = 1;

        /* pick satisfied: relax every app that lists it as a dependency */
        for (i = 0; i < n; i++) {
            if (done[i]) continue;
            for (k = 0; k < apps[i].ndeps; k++) {
                if (cfg_index(apps, n, apps[i].deps[k]) == pick) {
                    indeg[i]--;
                    break;
                }
            }
        }
    }

    free(indeg);
    free(done);
    *order_out = order;
    return 0;
}
