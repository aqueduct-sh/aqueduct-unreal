#pragma once

/**
 * Animated-graph playback tuning. These are fixed product invariants rather
 * than project settings. Per-graph cost controls belong in the graph itself.
 */
namespace AqueductAnimated
{
	/** Seconds an animated graph may go unsampled before releasing its
	 * working GPU memory. */
	constexpr float IdleReleaseSeconds = 5.0f;

	/** Maximum update rate for animated graphs. */
	constexpr float MaxFPS = 60.0f;
}
