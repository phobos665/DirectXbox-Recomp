# xbox_net: what System Link needs from the host's network beyond a socket --
# the router's port forward (UPnP, through third_party/miniupnp) and the
# addresses to give friends -- and registering a console with Insignia for
# Xbox Live (insignia_register.c). Used by the runtime (xbox_kernel) and the
# launcher alike.

add_library(xbox_net STATIC net_upnp.c insignia_register.c)
target_include_directories(xbox_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The User-Agent says which toolkit is asking.
target_compile_definitions(xbox_net PRIVATE XBOXRECOMP_VERSION="${xboxrecomp_VERSION}")

if(TARGET libminiupnpc-static)
    target_link_libraries(xbox_net PRIVATE libminiupnpc-static)
    target_compile_definitions(xbox_net PRIVATE XBOXRECOMP_HAVE_UPNP)
endif()

if(WIN32)
    target_link_libraries(xbox_net PUBLIC ws2_32 winhttp)
    if(MSVC)
        target_compile_options(xbox_net PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
    endif()
else()
    target_link_libraries(xbox_net PUBLIC ${CMAKE_DL_LIBS})   # libcurl, at run time
endif()
