# Upstream allnoconfig resets even KCONFIG_ALLCONFIG's booleans. Start with
# its complete disabled config, enable the agreed profile, then resolve it.
file(MAKE_DIRECTORY "${BUILD}")
execute_process(COMMAND ${MAKE} -C ${SOURCE} O=${BUILD} allnoconfig COMMAND_ERROR_IS_FATAL ANY)
set(options
    STATIC
    ASH
    ASH_BASH_COMPAT # Upstream gates pipefail (pipeline error propagation) here.
    SH_IS_ASH
    ASH_ECHO
    ASH_PRINTF
    ASH_TEST
    LS
    CAT
    MKDIR
    CP
    MV
    RM
    GREP
    WC
)
file(READ "${BUILD}/.config" config)
foreach(option IN LISTS options)
    string(REPLACE "# CONFIG_${option} is not set" "CONFIG_${option}=y" config "${config}")
endforeach()
file(WRITE "${BUILD}/.config" "${config}")
execute_process(COMMAND ${MAKE} -C ${SOURCE} O=${BUILD} oldconfig COMMAND_ERROR_IS_FATAL ANY)
file(READ "${BUILD}/.config" config)
foreach(option IN LISTS options)
    if(NOT config MATCHES "\nCONFIG_${option}=y\n")
        message(FATAL_ERROR "BusyBox configuration did not enable ${option}")
    endif()
endforeach()
