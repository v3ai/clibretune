(function () {
  "use strict";
  var REPO = "v3ai/clibretune";
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };

  /* Recommend the build for this computer. */
  var ua = navigator.userAgent || "";
  var plat = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "";
  var os = null, label = "Download";
  if (/Android|iPhone|iPad/i.test(ua)) { os = null; }
  else if (/Win/i.test(plat)) { os = "windows"; label = "Download for Windows"; }
  else if (/Mac/i.test(plat)) { os = "mac"; label = "Download for macOS"; }
  else if (/Linux|X11/i.test(plat + " " + ua)) {
    if (/aarch64|arm64/i.test(plat + " " + ua)) { os = "linux-arm"; label = "Download for Linux ARM64"; }
    else { os = "linux"; label = "Download for Linux"; }
  }
  if (os) {
    var row = $('.dl tr[data-os="' + os + '"]');
    if (row) {
      row.classList.add("match");
      row.parentNode.insertBefore(row, row.parentNode.firstChild);
      var hero = $("#hero-dl");
      hero.textContent = label;
      hero.href = $("a", row).href;
    }
  }

  /* Version and file sizes from the latest release. */
  if (window.fetch) {
    fetch("https://api.github.com/repos/" + REPO + "/releases/latest")
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (rel) {
        if (!rel) { return; }
        if (rel.tag_name) { $("#dl-version").textContent = rel.tag_name; }
        (rel.assets || []).forEach(function (a) {
          var cell = $('[data-size="' + a.name + '"]');
          if (cell) { cell.textContent = (a.size / 1e6).toFixed(1) + " MB"; }
        });
      })
      .catch(function () {});
  }

  /* Tuning session: swap the pinned screenshot as each step reaches the middle of the screen. */
  var steps = $$(".step");
  var stageImgs = $$(".stage img");
  var stageLink = $("#stage-link");
  function activate(step) {
    var key = step.getAttribute("data-img");
    steps.forEach(function (s) { s.classList.toggle("active", s === step); });
    stageImgs.forEach(function (img) { img.classList.toggle("on", img.getAttribute("data-key") === key); });
    var inline = $(".inline", step);
    if (stageLink && inline) {
      stageLink.href = inline.getAttribute("href");
      stageLink.setAttribute("data-cap", inline.getAttribute("data-cap"));
    }
  }
  if (steps.length) {
    var ticking = false;
    var pick = function () {
      ticking = false;
      var mid = window.innerHeight / 2, best = steps[0];
      steps.forEach(function (s) { if (s.getBoundingClientRect().top < mid) { best = s; } });
      if (!best.classList.contains("active")) { activate(best); }
    };
    window.addEventListener("scroll", function () {
      if (!ticking) { ticking = true; window.requestAnimationFrame(pick); }
    }, { passive: true });
    window.addEventListener("resize", pick);
    pick();
  }

  /* Click a screenshot to see it full size. */
  var box = $("#lightbox");
  if (box && typeof box.showModal === "function") {
    var big = $("#lb-img"), cap = $("#lb-cap");
    $$(".zoom").forEach(function (a) {
      a.addEventListener("click", function (ev) {
        ev.preventDefault();
        big.src = a.getAttribute("href");
        big.alt = a.getAttribute("data-cap") || "";
        cap.textContent = a.getAttribute("data-cap") || "";
        box.showModal();
      });
    });
    box.addEventListener("click", function (ev) { if (ev.target !== $(".lb-close")) { box.close(); } });
  }
})();
