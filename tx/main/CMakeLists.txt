set(srcs "main.c"
         "gatt_svr.c"
         "box_reverse.c"
         "box_protocol.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS ".")
