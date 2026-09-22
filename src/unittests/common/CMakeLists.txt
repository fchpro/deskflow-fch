# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

if(WIN32)
  create_test(NAME LeftModifierSwapPersistenceTests DEPENDS common
    SOURCE LeftModifierSwapPersistenceTests.cpp WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME ExcludedAppsPersistenceTests DEPENDS common
    SOURCE ExcludedAppsPersistenceTests.cpp WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()

create_test(
  NAME SettingsTests
  DEPENDS common
  SOURCE SettingsTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/common"
)

create_test(
  NAME LogLevelTests
  DEPENDS common
  SOURCE LogLevelTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/common"
)

create_test(
  NAME I18NTests
  DEPENDS common
  SOURCE I18NTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/common"
)
