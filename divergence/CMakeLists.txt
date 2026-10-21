add_executable(divergence
    divergence_main.cpp
    divergence.cpp
)

target_link_libraries(divergence PRIVATE libmim)

add_dependencies(divergence mim_all_plugins)
add_dependencies(mim_all divergence)
