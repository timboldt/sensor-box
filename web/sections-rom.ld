/* Linker section for the dashboard's HTTP resources (HTTP_RESOURCE_DEFINE in src/web.c). */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_dashboard, Z_LINK_ITERABLE_SUBALIGN)
