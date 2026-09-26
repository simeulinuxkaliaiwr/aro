/* logfile.h: aro's log, to stderr and to a file */
#ifndef ARO_LOGFILE_H
#define ARO_LOGFILE_H

#include <stdbool.h>
#include <wlr/util/log.h>

/* stderr + file; NULL = default path, "none" = no file */
void logfile_init(const char *path, bool nested,
                  enum wlr_log_importance verbosity);

/* the file being written, or NULL */
const char *logfile_path(void);

void logfile_finish(void);

#endif
