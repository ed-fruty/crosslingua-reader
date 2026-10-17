#pragma once

// Host stand-in for the firmware logger: Word Lens tests only assert behaviour, never log text.
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
