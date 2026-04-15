# Shim component: ESP-IDF v6.0 removed the built-in "json" component.
# esp-sr still depends on "json", so this wrapper delegates to espressif/cjson.
idf_component_register(REQUIRES espressif__cjson)
