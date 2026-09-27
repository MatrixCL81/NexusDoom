if(UNIX AND NOT APPLE)
    install(FILES nexusdoom.desktop DESTINATION share/applications)
    install(FILES nexusdoom.png DESTINATION share/icons/hicolor/512x512/apps)
endif()
