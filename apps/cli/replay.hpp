#pragma once
#include "console.hpp"
#include <streamcore/recording.hpp>

namespace streamdesk_cli {
// Uses an already validated reader. All state is scoped to this invocation.
int replay(streamcore::RecordingReader& reader, bool interactive, unsigned delay, CommandInput& input);
} // namespace streamdesk_cli
