#pragma once

#include <list>
#include <string>

// `run selftest <character> <second character>`: the run's move removal (MotionMaster::RemoveMove) and its run moves
// (GuildRunStates::StartRunMove), checked on a lab or bench character's own motion stack in the running core, with the
// second character as a chase target. On the console's world thread between map updates. Each case prints pass or fail
// with the stack before and after; the character is left with its stack cleared to idle.
std::list<std::string> GuildRunSelfTest(const std::string& args);
