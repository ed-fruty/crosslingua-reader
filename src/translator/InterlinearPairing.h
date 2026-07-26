#pragma once

#include <Epub/InterlinearAnnotation.h>

// The app half of the PtLayout::Interlinear boundary (see lib/Epub/Epub/InterlinearAnnotation.h).
//
// Wraps the shared sentence aligner (SentencePairing.h) in the plain-C-pointer signature the layout
// engine calls through, so lib/Epub never includes an app header. Handed to the engine by
// CrossPointSettings::readerRenderSpec() as ReaderRenderSpec::interlinearPairFn.
//
// Kept in its own TU rather than in SentencePairing.cpp so that file stays pure text logic and can
// be compiled by the `native` test env (which has neither lib/Epub nor lib/Memory on its include
// path).
int interlinearPairSentences(const char* const* sourceWords, int sourceWordCount, const char* const* translationWords,
                             int translationWordCount, InterlinearAnnotation* out, int maxOut);
