# SPDX-License-Identifier: MIT
# Out-of-tree driver tree for the sensor-box module.

add_subdirectory_ifdef(CONFIG_PMSA003I sensor/pmsa003i)
add_subdirectory_ifdef(CONFIG_SGP30    sensor/sgp30)
