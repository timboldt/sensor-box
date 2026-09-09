# SPDX-License-Identifier: MIT
# Out-of-tree driver tree for the sensor-box module.

# Public headers for the out-of-tree drivers (e.g. <sensor_box/sgp30.h>).
zephyr_include_directories(include)

add_subdirectory_ifdef(CONFIG_PMSA003I sensor/pmsa003i)
add_subdirectory_ifdef(CONFIG_SGP30    sensor/sgp30)
