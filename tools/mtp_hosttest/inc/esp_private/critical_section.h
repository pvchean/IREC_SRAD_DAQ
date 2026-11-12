#pragma once
#define DEFINE_CRIT_SECTION_LOCK_STATIC(name) static int name
#define esp_os_enter_critical(l) ((void)(l))
#define esp_os_exit_critical(l) ((void)(l))
