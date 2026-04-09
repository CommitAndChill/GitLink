#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// Progress + cancellation surface for long-running git operations (Fetch / Pull / Push / Clone).
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	// --------------------------------------------------------------------------------------------------------------------
	// Snapshot of a fetch/push in progress. Mirrors libgit2's git_indexer_progress but with plain integers.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FProgressInfo
	{
		uint32 TotalObjects    = 0;
		uint32 ReceivedObjects = 0;
		uint32 IndexedObjects  = 0;
		uint64 ReceivedBytes   = 0;

		FString Stage;   // "Fetching", "Resolving deltas", "Uploading", etc.

		auto Get_Percent() const -> float
		{
			if (TotalObjects == 0)
			{ return 0.0f; }

			return (float)ReceivedObjects / (float)TotalObjects;
		}
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Called periodically from a worker thread during a long-running operation.
	// Return false to request cancellation; the operation will unwind with an error.
	// --------------------------------------------------------------------------------------------------------------------
	using FProgressCallback = TFunction<bool(const FProgressInfo& /*InInfo*/)>;
}
