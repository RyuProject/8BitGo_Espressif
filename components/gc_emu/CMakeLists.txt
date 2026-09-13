idf_component_register(
    SRCS "gc_emu.c" "gc_emu_nes.c" "gc_emu_gb.cpp"
    INCLUDE_DIRS "include"
    REQUIRES gc_api gc_prefs gc_gambatte gc_sys
)
