import { formatDateTimeAsMoment } from "@/utils/directories";
import { formatDuration, numberWithCommas } from "@/utils/numbers";
import React, { useEffect, useState, useContext } from "react";
import { isMobile } from "react-device-detect";
const MetricsContext = React.createContext();
const SHOW_METRICS_KEY = "anythingllm_show_chat_metrics";
const SHOW_METRICS_EVENT = "anythingllm_show_metrics_change";

/**
 * Format the output TPS to a string
 * @param {number} outputTps - output TPS
 * @returns {string}
 */
function formatTps(outputTps) {
  try {
    return outputTps < 1000
      ? outputTps.toFixed(2)
      : numberWithCommas(outputTps.toFixed(0));
  } catch {
    return "";
  }
}

/**
 * Get the show metrics setting from localStorage `anythingllm_show_chat_metrics` key
 * @returns {boolean}
 */
function getAutoShowMetrics() {
  return window?.localStorage?.getItem(SHOW_METRICS_KEY) === "true";
}

/**
 * Build the metrics string for a given metrics object
 * - Model name
 * - Duration and output TPS
 * - Timestamp
 * @param {metrics: {duration:number, outputTps: number, model?: string, timestamp?: number}} metrics
 * @returns {string}
 */
function buildMetricsString(metrics = {}) {
  return [
    metrics?.model ? metrics.model : "",
    `${formatDuration(metrics.duration)} (${formatTps(metrics.outputTps)} tok/s)`,
    metrics?.timestamp
      ? formatDateTimeAsMoment(metrics.timestamp, "MMM D, h:mm A")
      : "",
  ]
    .filter(Boolean)
    .join(" · ");
}

/**
 * Build a detailed, separately readable timing line. Individual values may be
 * absent when no vector search occurred (for example, a workspace without docs).
 * @param {object} metrics
 * @returns {string}
 */
function buildTimingString(metrics = {}) {
  const timing = metrics?.timing || {};
  return [
    timing.client_e2e_ms != null ? `E2E ${timing.client_e2e_ms}ms` : null,
    timing.server_total_ms != null
      ? `Server ${timing.server_total_ms}ms`
      : null,
    timing.query_embedding_ms != null
      ? `Embed ${timing.query_embedding_ms}ms`
      : null,
    timing.vector_retrieval_ms != null
      ? `Retrieval ${timing.vector_retrieval_ms}ms`
      : null,
    timing.vector_db_query_ms != null
      ? `Vector DB ${timing.vector_db_query_ms}ms`
      : null,
    timing.qcovert_core_search_ms != null
      ? `Q-COVERT core ${timing.qcovert_core_search_ms}ms`
      : null,
  ]
    .filter(Boolean)
    .join(" · ");
}

/**
 * Toggle the show metrics setting in localStorage `anythingllm_show_chat_metrics` key
 * @returns {void}
 */
function toggleAutoShowMetrics() {
  const currentValue = getAutoShowMetrics() || false;
  window?.localStorage?.setItem(SHOW_METRICS_KEY, !currentValue);
  window.dispatchEvent(
    new CustomEvent(SHOW_METRICS_EVENT, {
      detail: { showMetricsAutomatically: !currentValue },
    })
  );
  return !currentValue;
}

/**
 * Provider for the metrics context that controls the visibility of the metrics
 * per-chat based on the user's preference.
 * @param {React.ReactNode} children
 * @returns {React.ReactNode}
 */
export function MetricsProvider({ children }) {
  const [showMetricsAutomatically, setShowMetricsAutomatically] =
    useState(getAutoShowMetrics());

  useEffect(() => {
    function handleShowingMetricsEvent(e) {
      if (!e?.detail?.hasOwnProperty("showMetricsAutomatically")) return;
      setShowMetricsAutomatically(e.detail.showMetricsAutomatically);
    }
    console.log("Adding event listener for metrics visibility");
    window.addEventListener(SHOW_METRICS_EVENT, handleShowingMetricsEvent);
    return () =>
      window.removeEventListener(SHOW_METRICS_EVENT, handleShowingMetricsEvent);
  }, []);

  return (
    <MetricsContext.Provider
      value={{ showMetricsAutomatically, setShowMetricsAutomatically }}
    >
      {children}
    </MetricsContext.Provider>
  );
}

/**
 * Render the metrics for a given chat, if available
 * @param {metrics: {duration:number, outputTps: number, model: string, timestamp: number}} props
 * @returns
 */
export default function RenderMetrics({ metrics = {} }) {
  // Inherit the showMetricsAutomatically state from the MetricsProvider so the state is shared across all chats
  const { showMetricsAutomatically, setShowMetricsAutomatically } =
    useContext(MetricsContext);
  if (!metrics?.duration || !metrics?.outputTps || isMobile) return null;
  const timing = buildTimingString(metrics);

  return (
    <button
      type="button"
      onClick={() => setShowMetricsAutomatically(toggleAutoShowMetrics())}
      data-tooltip-id="metrics-visibility"
      data-tooltip-content={
        showMetricsAutomatically
          ? "Click to only show metrics when hovering"
          : "Click to show metrics as soon as they are available"
      }
      className={`border-none flex md:justify-end items-center gap-x-[8px] -ml-7 ${showMetricsAutomatically ? "opacity-100" : "opacity-0"} md:group-hover:opacity-100 transition-all duration-300`}
    >
      <span className="cursor-pointer text-xs font-mono text-zinc-400 light:text-slate-500 text-right">
        <span className="block">{buildMetricsString(metrics)}</span>
        {timing && (
          <span className="block mt-1 text-[10px] text-emerald-500 light:text-emerald-600">
            {timing}
          </span>
        )}
      </span>
    </button>
  );
}
