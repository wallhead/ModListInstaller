(function () {
  "use strict";

  const dom = {
    stepLabel: document.getElementById("stepLabel"),
    versionText: document.getElementById("versionText"),
    footerInfo: document.getElementById("footerInfo"),
    unpackDrive: document.getElementById("unpackDrive"),
    unpackTarget: document.getElementById("unpackTarget"),
    installFolder: document.getElementById("installFolder"),
    progressFill: document.getElementById("progressFill"),
    percentText: document.getElementById("percentText"),
    statusText: document.getElementById("statusText"),
    logOutput: document.getElementById("logOutput"),
    backButton: document.getElementById("backButton"),
    nextButton: document.getElementById("nextButton"),
    startButton: document.getElementById("startButton"),
    cancelButton: document.getElementById("cancelButton"),
    browseInstall: document.getElementById("browseInstall"),
    openLog: document.getElementById("openLog")
  };

  function send(command, payload) {
    const message = Object.assign({ command }, payload || {});
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(message);
    }
  }

  function clampPercent(value) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) {
      return 0;
    }
    return Math.max(0, Math.min(100, Math.round(numeric)));
  }

  function setStep(stepName) {
    dom.stepLabel.textContent = stepName ? `Current phase: ${stepName}` : "Local package installer";
    if (dom.startButton) {
      dom.startButton.hidden = false;
    }
  }

  function setPath(fieldName, value) {
    if (fieldName === "installFolder") {
      dom.installFolder.value = value || "";
    } else if (fieldName === "unpackTarget") {
      dom.unpackTarget.textContent = value || "Target: choose a drive";
    } else if (fieldName === "unpackDrive") {
      dom.unpackDrive.value = value || "";
    }
  }

  function setProgress(percent, status) {
    const clamped = clampPercent(percent);
    dom.progressFill.style.width = `${clamped}%`;
    dom.percentText.textContent = `${clamped}%`;
    dom.percentText.parentElement.nextElementSibling.setAttribute("aria-valuenow", String(clamped));
    if (typeof status === "string" && status.length > 0) {
      setStatus(status);
    }
  }

  function setStatus(status) {
    dom.statusText.textContent = status || "";
    dom.footerInfo.textContent = status || "";
  }

  function addLog(message) {
    if (!message) {
      return;
    }
    dom.logOutput.value += `${message}\n`;
    dom.logOutput.scrollTop = dom.logOutput.scrollHeight;
  }

  function setButtonEnabled(buttonName, enabled) {
    const map = {
      back: dom.backButton,
      next: dom.nextButton,
      start: dom.startButton,
      cancel: dom.cancelButton,
      browseInstall: dom.browseInstall
    };
    if (map[buttonName]) {
      map[buttonName].disabled = !enabled;
    }
  }

  function showError(title, message) {
    addLog(`${title || "Error"}: ${message || ""}`);
  }

  function applyState(state) {
    if (!state || typeof state !== "object") {
      return;
    }
    if (Array.isArray(state.drives)) {
      dom.unpackDrive.replaceChildren(...state.drives.map((drive) => {
        const option = document.createElement("option");
        option.value = drive;
        option.textContent = drive;
        return option;
      }));
    }
    setStep(state.step || "Welcome");
    setPath("installFolder", state.installFolder);
    setPath("unpackDrive", state.unpackDrive);
    setPath("unpackTarget", state.unpackTarget);
    setProgress(state.progress || 0, state.status || "");
    dom.logOutput.value = "";
    (state.logs || []).forEach(addLog);
    dom.versionText.textContent = state.version || "Local package mode";
    setButtonEnabled("back", !!state.buttons?.back);
    setButtonEnabled("next", !!state.buttons?.next);
    setButtonEnabled("start", !!state.buttons?.start);
    setButtonEnabled("cancel", !!state.buttons?.cancel);
    setButtonEnabled("browseInstall", !!state.buttons?.browseInstall);
  }

  window.installerUi = {
    setProgress,
    setStatus,
    addLog,
    setStep,
    setPath,
    setOption() {},
    showError,
    setButtonEnabled,
    applyState
  };

  dom.browseInstall.addEventListener("click", () => send("browseInstallFolder"));
  dom.backButton?.addEventListener("click", () => send("previousStep"));
  dom.nextButton?.addEventListener("click", () => send("nextStep"));
  dom.startButton.addEventListener("click", () => send("startInstall"));
  dom.cancelButton.addEventListener("click", () => send("cancelInstall"));
  dom.openLog.addEventListener("click", () => send("openLog"));
  dom.installFolder.addEventListener("change", () => send("setPath", { name: "installFolder", value: dom.installFolder.value }));
  dom.unpackDrive.addEventListener("change", () => send("setPath", { name: "unpackDrive", value: dom.unpackDrive.value }));

  window.chrome?.webview?.addEventListener("message", (event) => {
    const message = typeof event.data === "string" ? JSON.parse(event.data) : event.data;
    if (!message || typeof message !== "object") {
      return;
    }
    if (message.type === "state") {
      applyState(message.state);
    } else if (message.type === "progress") {
      setProgress(message.percent, message.status);
    } else if (message.type === "status") {
      setStatus(message.status);
    } else if (message.type === "log") {
      addLog(message.message);
    } else if (message.type === "step") {
      setStep(message.step);
    } else if (message.type === "path") {
      setPath(message.name, message.value);
    } else if (message.type === "option") {
      window.installerUi.setOption(message.name, message.value);
    } else if (message.type === "error") {
      showError(message.title, message.message);
    } else if (message.type === "button") {
      setButtonEnabled(message.name, message.enabled);
    }
  });

  send("uiReady");
}());
