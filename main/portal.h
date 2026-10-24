#pragma once

#include <stdbool.h>

/* Start the configuration web server (and mDNS). With captive=true a catch-all DNS server and
 * redirects are enabled so phones open the setup page automatically. */
void portal_start(bool captive);
