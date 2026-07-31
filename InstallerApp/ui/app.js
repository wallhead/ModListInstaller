(function () {
  "use strict";

  const dom = {
    stepLabel: document.getElementById("stepLabel"),
    versionText: document.getElementById("versionText"),
    unpackDrive: document.getElementById("unpackDrive"),
    unpackTarget: document.getElementById("unpackTarget"),
    installFolder: document.getElementById("installFolder"),
    finalInstallFolder: document.getElementById("finalInstallFolder"),
    progressFill: document.getElementById("progressFill"),
    percentText: document.getElementById("percentText"),
    statusText: document.getElementById("statusText"),
    logOutput: document.getElementById("logOutput"),
    backButton: document.getElementById("backButton"),
    nextButton: document.getElementById("nextButton"),
    startButton: document.getElementById("startButton"),
    cancelButton: document.getElementById("cancelButton"),
    browseInstall: document.getElementById("browseInstall"),
    openLog: document.getElementById("openLog"),
    messageModal: document.getElementById("messageModal"),
    messageTitle: document.getElementById("messageTitle"),
    messageText: document.getElementById("messageText"),
    messageCancel: document.getElementById("messageCancel"),
    messageOk: document.getElementById("messageOk")
  };
  let messageAction = "";

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

  function setStep() {
    dom.stepLabel.textContent = "Local package installer";
    if (dom.startButton) {
      dom.startButton.hidden = false;
    }
  }

  function setPath(fieldName, value) {
    if (fieldName === "installFolder") {
      dom.installFolder.value = value || "";
    } else if (fieldName === "unpackTarget") {
      dom.unpackTarget.textContent = value || "Выберите диск";
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
    addLog(`${title || "Ошибка"}: ${message || ""}`);
    showMessageModal(title || "Ошибка", message || "");
  }

  function showMessageModal(title, message, action) {
    messageAction = action || "";
    dom.messageTitle.textContent = title || "";
    dom.messageText.textContent = message || "";
    dom.messageText.hidden = !message;
    dom.messageCancel.hidden = messageAction !== "overwrite";
    dom.messageOk.textContent = messageAction === "overwrite" ? "Перезаписать" : "OK";
    dom.messageModal.hidden = false;
    dom.messageOk.focus();
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
    setStep(state.step || "Добро пожаловать");
    setPath("installFolder", state.installFolder);
    dom.finalInstallFolder.textContent = state.finalInstallFolder || "Выберите папку установки";
    setPath("unpackDrive", state.unpackDrive);
    setPath("unpackTarget", state.unpackTarget);
    setProgress(state.progress || 0, state.status || "");
    dom.logOutput.value = "";
    (state.logs || []).forEach(addLog);
    dom.versionText.textContent = state.version || "Local package mode";
    dom.startButton.textContent = state.installCompleted ? "Закрыть" : "Установить";
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
  dom.messageOk.addEventListener("click", () => {
    const action = messageAction;
    messageAction = "";
    dom.messageModal.hidden = true;
    if (action === "overwrite") {
      send("confirmOverwrite");
    }
  });
  dom.messageCancel.addEventListener("click", () => {
    const action = messageAction;
    messageAction = "";
    dom.messageModal.hidden = true;
    if (action === "overwrite") {
      send("cancelOverwrite");
    }
  });
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
    } else if (message.type === "overwriteConfirm") {
      showMessageModal(message.title, message.message, "overwrite");
    } else if (message.type === "button") {
      setButtonEnabled(message.name, message.enabled);
    } else if (message.type === "installComplete") {
      showMessageModal("Установка завершена!", "");
    }
  });

  send("uiReady");
}());
