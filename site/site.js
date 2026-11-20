(function () {
  "use strict";

  /* screenshot tabs */
  var shots = {
    "dashboard": ["Dashboard", "Gauges driven by the built-in simulated ECU."],
    "ve-table": ["VE table", "32 x 32 VE table with heat map, live-cell tracking and the full editing toolbar."],
    "autotune": ["AutoTune", "AutoTune recommendations with per-region health scores."],
    "datalog": ["Data log", "Live data logging; pick any of the ECU's output channels."],
    "terminal": ["Terminal UI", "The same app in a terminal: --backend tty."]
  };
  var img = document.getElementById("shot");
  var title = document.getElementById("shot-title");
  var cap = document.getElementById("shot-caption");
  var tabs = document.querySelectorAll(".tabs button");
  Array.prototype.forEach.call(tabs, function (b) {
    b.addEventListener("click", function () {
      var k = b.getAttribute("data-shot");
      Array.prototype.forEach.call(tabs, function (o) { o.setAttribute("aria-selected", o === b ? "true" : "false"); });
      img.src = "img/" + k + ".webp";
      img.alt = "LibreTune " + shots[k][0];
      title.textContent = shots[k][0];
      cap.textContent = shots[k][1];
    });
  });
  Object.keys(shots).forEach(function (k) { new Image().src = "img/" + k + ".webp"; });

  /* highlight the download for this OS and point the hero button at it */
  var ua = navigator.userAgent || "";
  var plat = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "";
  var os = null, label = null;
  if (/Win/i.test(plat) || /Windows/i.test(ua)) { os = "windows"; label = "Download for Windows"; }
  else if (/Mac/i.test(plat) && !/iPhone|iPad/i.test(ua)) { os = "mac"; label = "Download for macOS"; }
  else if (/aarch64|arm64/i.test(plat + ua) && /Linux/i.test(plat + ua) && !/Android/i.test(ua)) { os = "linux-arm"; label = "Download for Linux ARM64"; }
  else if (/Linux|X11/i.test(plat + ua) && !/Android/i.test(ua)) { os = "linux"; label = "Download for Linux"; }
  if (os) {
    var card = document.querySelector('.dl[data-os="' + os + '"]');
    if (card) {
      card.classList.add("match");
      var hero = document.getElementById("hero-dl");
      hero.href = card.href;
      document.getElementById("hero-dl-label").textContent = label;
    }
  }

  /* show the latest release tag */
  if (window.fetch) {
    fetch("https://api.github.com/repos/v3ai/clibretune/releases/latest")
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (j) {
        if (!j || !j.tag_name) { return; }
        Array.prototype.forEach.call(document.querySelectorAll("[data-version]"), function (el) { el.textContent = j.tag_name; });
        document.getElementById("rel-link").textContent = j.tag_name + " release";
      })
      .catch(function () {});
  }
})();
