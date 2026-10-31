#pragma once

#include <string>

/* Which display the captured game is on, used to suggest (or, in automatic
 * mode, pick) the display whose refresh rate the recording should match. */
struct GameDisplay {
	enum class Source {
		GameWindow, // window of a Game Capture source
		Foreground, // foreground window of another application
		Primary,    // nothing better found
	};

	std::wstring device; // e.g. \\.\DISPLAY1
	std::wstring executable;
	Source source = Source::Primary;
};

GameDisplay FindGameDisplay();
