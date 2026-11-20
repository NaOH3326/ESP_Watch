set(reqs "watchshell" "factorytest")

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${reqs})
