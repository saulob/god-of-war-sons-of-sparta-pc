// How a session went, derived from a report's log text: the same rules as launcher/session_outcome.h, checked against
// the same cases (tests/fixtures/session_outcome_cases.txt). It is computed when the dashboard reads a report, so old
// reports get it too and the launcher sends no new field. A log that just stops is never called a crash.

// Raise when a rule changes: stored outcomes of older rules are classified again when the service starts.
export const SESSION_OUTCOME_RULES = 3;

export const SESSION_OUTCOMES = [
  'launcher_no_play', 'play_blocked', 'graphics_preflight_failed', 'settings_save_failed', 'launcher_close_hang',
  'runtime_handoff_failed', 'startup_hang', 'startup_crash', 'black_screen', 'device_lost', 'memory_failure', 'crash',
  'normal_exit', 'ended_unexpectedly', 'unknown',
];

const LABELS = {
  launcher_no_play: 'Launcher open, no PLAY attempt',
  play_blocked: 'PLAY requested but blocked',
  graphics_preflight_failed: 'Graphics check failed',
  settings_save_failed: 'Settings could not be saved',
  launcher_close_hang: 'Hang while the launcher closed',
  runtime_handoff_failed: 'Game did not start after the launcher closed',
  startup_hang: 'Hang while the game started',
  startup_crash: 'Crash while the game started',
  black_screen: 'Black screen (frames made, none shown)',
  device_lost: 'Vulkan device lost',
  memory_failure: 'Memory allocation failure',
  crash: 'Crash during play',
  normal_exit: 'Normal exit',
  ended_unexpectedly: 'Ended without a closing record',
  unknown: 'Not enough information',
};

export const sessionOutcomeLabel = (key) => LABELS[key] ?? LABELS.unknown;

const NO_RECORD = 'the log ends without a closing record (the process was killed, hung or ended without a report)';
const MEMORY = ['ErrorOutOfDeviceMemory', 'ErrorOutOfHostMemory', 'OUT_OF_DEVICE_MEMORY', 'OUT_OF_HOST_MEMORY', 'Out of memory'];
const isMemoryLine = (line) => MEMORY.some((token) => line.includes(token));

function stageOf(line) {
  const rest = line.slice(7);
  for (const end of [' started +', ' completed', ' failed']) {
    const at = rest.indexOf(end);
    if (at >= 0) return rest.slice(0, at);
  }
  return rest;
}

const joined = (a, b) => (!a ? b : !b ? a : `${a}; ${b}`);

export function analyzeSessionOutcome(log) {
  const s = {
    launcherShown: false, play: false, playStarting: false, launcherClosed: false, handoff: false, gameChild: false,
    firstPresent: false, firstFlip: false, normalExit: false, quitRequested: false, crash: false, fatalStop: false,
    memoryNearEnd: false, blackScreen: false, deviceLost: false, watchdog: false, endedBySignal: false,
  };
  let blocked = '';
  let lastStage = '';
  let watchdogStage = '';
  let signalLine = '';
  let trace = ''; // the GPU trace summary of a device loss or GPU assertion, without its prefix
  // A closing record counts for the game only after the handoff ("Launcher: closed, starting the game" is in every
  // in-process start). Logs with no Stage/Session line come from before 0.1.1, which wrote no closing record.
  let gameQuit = false;
  let modern = false;
  const recent = [];

  for (let line of String(log ?? '').split('\n')) {
    if (line.endsWith('\r')) line = line.slice(0, -1);
    if (!line) continue;
    if (line.startsWith('Stage: ')) lastStage = stageOf(line);
    if (line.startsWith('Stage') || line.startsWith('Session: ')) modern = true;
    if (line === 'Launcher: shown') s.launcherShown = true;
    if (line.startsWith('Stage event: PLAY requested') || line === 'Launcher: settings saved, starting the game') {
      s.play = true;
      blocked = '';
    }
    if (line.startsWith('Stage: play: starting started') || line === 'Launcher: settings saved, starting the game') {
      s.playStarting = true;
    }
    if (line.startsWith('Stage event: PLAY blocked: ')) {
      blocked = line.slice(27);
      const plus = blocked.lastIndexOf(' +');
      if (plus >= 0) blocked = blocked.slice(0, plus);
    }
    if (line.startsWith('Stage: play: settings save failed')) blocked = 'settings save failed';
    if (line.startsWith('Launcher: closed')) s.launcherClosed = true;
    if (line.includes('quit requested by the system') || line === 'Launcher: window close requested') {
      s.quitRequested = true;
      if (s.handoff) gameQuit = true;
    }
    if (line.startsWith('Stage event: Runtime handoff') || line.startsWith('Startup: game-only child process') ||
        line.startsWith('Launcher: the game process continues this log') || line.startsWith('Overlay compatibility: ')) {
      s.handoff = true;
    }
    if (line.startsWith('Startup: game-only child process')) s.gameChild = true;
    // Play starts with the first frame of the game itself ("Runtime: first present", also in 0.1.0 logs), not with
    // "Startup: first present", which is the renderer's loading screen before the guest even starts.
    if (line === 'Runtime: first present' || line.startsWith('Stage: game running started')) s.firstPresent = true;
    if (line.startsWith('Runtime: first flip taken') || line.startsWith('Runtime: first flip prepared')) s.firstFlip = true;
    if (line.startsWith('Session: normal exit')) s.normalExit = true;
    if (line.startsWith('Session: ended by signal')) {
      s.endedBySignal = true;
      signalLine = line;
    }
    if (line.startsWith('Presenter diagnostic: no successful present')) s.blackScreen = true;
    if (line.startsWith('Presenter diagnostic: presenting resumed')) s.blackScreen = false;
    if (line.startsWith('GPU device lost: first detected in ')) s.deviceLost = true;
    if (line.startsWith('Stage watchdog: ')) {
      s.watchdog = true;
      const rest = line.slice(16);
      const at = rest.indexOf(' still running');
      watchdogStage = at >= 0 ? rest.slice(0, at) : rest;
    }
    const fatal = line.startsWith('CRASH:') || line.startsWith('STOP: ');
    if (fatal) {
      if (recent.some(isMemoryLine) || isMemoryLine(line)) s.memoryNearEnd = true;
      if (line.startsWith('CRASH:')) s.crash = true;
      else s.fatalStop = true;
    }
    if (!trace && line.startsWith('GPU trace: summary: ')) trace = line.slice(20);
    // The GPU trace (up to 1000 lines) is printed between the failure and its stop line: it is not evidence of its own
    // and must not push the failure out of the window.
    if (line.startsWith('GPU trace: ')) continue;
    recent.push(line);
    if (recent.length > 40) recent.shift();
  }
  if (!s.crash && !s.fatalStop && recent.some((line) => isMemoryLine(line) && line.includes('fatal'))) s.memoryNearEnd = true;

  const closedRecord = s.normalExit || s.launcherClosed || s.quitRequested || s.endedBySignal;
  const where = lastStage ? `last stage: ${lastStage}` : '';
  const out = (key, detail) => ({ key, label: sessionOutcomeLabel(key), detail });

  const traced = trace ? `GPU trace: ${trace}` : '';
  if (s.deviceLost) return out('device_lost', joined(where, traced));
  if ((s.crash || s.fatalStop) && s.memoryNearEnd) return out('memory_failure', joined(where, traced));
  if (s.crash || s.fatalStop) {
    return out(s.firstPresent ? 'crash' : 'startup_crash',
      joined(joined(where, s.fatalStop && !s.crash ? 'fatal stop' : 'crash report written'), traced));
  }
  if (s.blackScreen) return out('black_screen', where);
  if (s.memoryNearEnd) return out('memory_failure', where);

  const game = s.handoff || s.gameChild || s.firstPresent;
  if (!game) {
    if (!s.launcherShown && !s.play) return out('unknown', joined(where, closedRecord ? '' : NO_RECORD));
    if (!s.play) return out('launcher_no_play', joined(where, closedRecord ? 'the launcher was closed' : NO_RECORD));
    if (blocked && !s.playStarting) {
      if (blocked.startsWith('graphics preflight')) return out('graphics_preflight_failed', blocked);
      if (blocked.startsWith('settings save')) return out('settings_save_failed', blocked);
      return out('play_blocked', blocked);
    }
    if (s.playStarting && !s.launcherClosed) {
      return out('launcher_close_hang', joined(where, s.watchdog ? `watchdog: ${watchdogStage}` : NO_RECORD));
    }
    if (s.launcherClosed) return out('runtime_handoff_failed', joined(where, closedRecord ? '' : NO_RECORD));
    return out('play_blocked', joined(where, 'PLAY did not start the game'));
  }
  const gameClosed = s.normalExit || gameQuit || s.endedBySignal;
  if (s.normalExit) return out('normal_exit', where);
  if (!s.firstPresent) {
    if (s.watchdog || !gameClosed) {
      return out(s.firstFlip ? 'black_screen' : 'startup_hang', joined(where, s.watchdog ? `watchdog: ${watchdogStage}` : NO_RECORD));
    }
    return out('startup_hang', joined(where, s.endedBySignal ? signalLine : 'closed before the first frame'));
  }
  if (s.endedBySignal) return out('ended_unexpectedly', joined(where, signalLine));
  if (gameQuit) return out('normal_exit', joined(where, 'quit requested by the system'));
  if (!modern) return out('unknown', joined(where, 'a log from a version before 0.1.1, which wrote no closing record'));
  return out('ended_unexpectedly', joined(where, NO_RECORD));
}
