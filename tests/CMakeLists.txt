add_executable(unit_tests unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE voy_core)
add_test(NAME unit COMMAND unit_tests)

# Game tests need the player's game folder: VOY_GAME_DIR in the environment.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(t attract play service)
  add_test(NAME game_${t} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/game_test.py $<TARGET_FILE:vgrun> ${t})
  set_tests_properties(game_${t} PROPERTIES SKIP_RETURN_CODE 77 LABELS game TIMEOUT 1800)
endforeach()
