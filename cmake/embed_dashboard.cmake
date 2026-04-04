# embed_dashboard.cmake — invoked by add_custom_command at build time
# Reads DASHBOARD_HTML, injects into DASHBOARD_IN template, writes DASHBOARD_OUT.
file(READ "${DASHBOARD_HTML}" DASHBOARD_HTML_RAW)
configure_file("${DASHBOARD_IN}" "${DASHBOARD_OUT}" @ONLY)
