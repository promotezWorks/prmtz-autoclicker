#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void prmtz_gui_init(double initial_cps);
int prmtz_gui_poll_messages(void);
void prmtz_gui_cleanup(void);
double prmtz_get_ram_usage_mb(void);
long long prmtz_get_click_count(void);

#ifdef __cplusplus
}
#endif
