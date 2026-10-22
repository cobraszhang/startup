/* topo.c - Kahn's topological sort over the dependency graph,
 * partitioned by startup class (system phase strictly before the
 * application phase).
 *
 * Deterministic: among apps whose dependencies are all satisfied,
 * the one listed earliest in the configuration is started first.
 * Portable C89.
 */
#include <stdlib.h>
#include <string.h>

#include "topo.h"
#include "log.h"

int topo_sort(const app_t *apps, int n, int kind,
              int **order_out, int *cnt_out)
{
    int *indeg, *done, *order;
    int i, j, k, pick, produced = 0;
    int ph, first_ph, last_ph, target, done_in_ph;

    *order_out = NULL;
    *cnt_out = 0;
    if (n <= 0) return 0;

    indeg = (int *)calloc((size_t)n, sizeof(int));
    done  = (int *)calloc((size_t)n, sizeof(int));
    order = (int *)malloc((size_t)n * sizeof(int));
    if (!indeg || !done || !order) {
        free(indeg); free(done); free(order);
        return -3;
    }

    /* validate every dependency name and the class rule direction */
    for (i = 0; i < n; i++) {
        for (k = 0; k < apps[i].ndeps; k++) {
            j = cfg_index(apps, n, apps[i].deps[k]);
            if (j < 0) {
                log_msg("topo: app '%s' depends on unknown app '%s'",
                        apps[i].name, apps[i].deps[k]);
                free(indeg); free(done); free(order);
                return -1;
            }
            if (app_kind(&apps[i]) == KIND_SYSTEM &&
                app_kind(&apps[j]) == KIND_APP) {
                log_msg("topo: system app '%s' must not depend on "
                        "application-class app '%s' (user apps start "
                        "after system services by design)",
                        apps[i].name, apps[j].name);
                free(indeg); free(done); free(order);
                return -4;
            }
        }
    }

    first_ph = (kind == TOPO_ALL) ? KIND_SYSTEM : kind;
    last_ph  = (kind == TOPO_ALL) ? KIND_APP    : kind;

    for (ph = first_ph; ph <= last_ph; ph++) {
        memset(indeg, 0, (size_t)n * sizeof(int));

        target = 0;
        for (i = 0; i < n; i++) {
            if (app_kind(&apps[i]) != ph) continue;
            target++;
            /* only edges inside this phase constrain the order;
               dependencies already served by an earlier phase count
               as satisfied */
            for (k = 0; k < apps[i].ndeps; k++) {
                j = cfg_index(apps, n, apps[i].deps[k]);
                if (app_kind(&apps[j]) == ph) indeg[i]++;
            }
        }

        for (done_in_ph = 0; done_in_ph < target; done_in_ph++) {
            pick = -1;
            for (i = 0; i < n; i++) {
                if (done[i] || app_kind(&apps[i]) != ph) continue;
                if (indeg[i] == 0) { pick = i; break; }
            }
            if (pick < 0) {
                log_msg("topo: dependency cycle detected among:");
                for (i = 0; i < n; i++)
                    if (!done[i] && app_kind(&apps[i]) == ph)
                        log_msg("topo:   %s", apps[i].name);
                free(indeg); free(done); free(order);
                return -2;
            }
            order[produced++] = pick;
            done[pick] = 1;

            /* pick satisfied: relax every app in this phase that
               lists it as a dependency */
            for (i = 0; i < n; i++) {
                if (done[i] || app_kind(&apps[i]) != ph) continue;
                for (k = 0; k < apps[i].ndeps; k++) {
                    if (cfg_index(apps, n, apps[i].deps[k]) == pick) {
                        indeg[i]--;
                        break;
                    }
                }
            }
        }
    }

    free(indeg);
    free(done);
    *order_out = order;
    *cnt_out = produced;
    return 0;
}
