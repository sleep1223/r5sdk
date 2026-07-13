cmake_minimum_required( VERSION 3.16 )
add_module( "lib" "networksystem" "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "bansystem.cpp"
    "bansystem.h"
    "clientdisconnect.cpp"
    "clientdisconnect.h"
    "disconnectmessage.cpp"
    "disconnectmessage.h"
    "hostmanager.cpp"
    "hostmanager.h"
    "listmanager.cpp"
    "listmanager.h"
    "matchreport.cpp"
    "matchreport.h"
    "playeraccess.cpp"
    "playeraccess.h"
    "pylon.cpp"
    "pylon.h"
    "remoteapi.cpp"
    "remoteapi.h"
)

add_sources( SOURCE_GROUP "Public"
    "serverlisting.h"
    "sm_protocol.h"
)

end_sources()
