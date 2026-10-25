/* Enhanced options (options.c): <user>/options.txt, overridable with --option key=value. */
#pragma once

void options_load(void);
int option_parse(const char *line);                     /* "key = value" */
const char *option_str(const char *key, const char *def);
int option_bool(const char *key, int def);
void option_set(const char *key, const char *value);   /* for this run; options_save() keeps it */
int options_save(void);         /* rewrite options.txt: changed keys in place, new ones appended */
void options_changed(void);     /* the in-game Enhanced page changed something: apply it now */
