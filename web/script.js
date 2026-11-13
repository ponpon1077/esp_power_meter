function toggleCard(header) {
  const card = header.parentElement;
  card.classList.toggle("open");
}

document.addEventListener("DOMContentLoaded", () => {
  const p1El = document.getElementById("p1");
  const i1El = document.getElementById("i1");
  const p2El = document.getElementById("p2");
  const i2El = document.getElementById("i2");
  const vEl = document.getElementById("v");
  const statusEl = document.getElementById("status");
  const optoBadge = document.getElementById("optoBadge");
  const periodSelect = document.getElementById("periodSelect");
  const btnUploadWeb = document.getElementById("btnUploadWeb");
  const webFilesInput = document.getElementById("webFiles");
  const uploadStatus = document.getElementById("uploadStatus");
  const littlefsFiles = document.getElementById("littlefsFiles");
  const btnUploadOta = document.getElementById("btnUploadOta");
  const otaFileInput = document.getElementById("otaFile");
  const otaStatus = document.getElementById("otaStatus");
  const ctx = document.getElementById("powerChart").getContext("2d");
  const powerChart = new Chart(ctx, {
    type: "line",
    data: {
      labels: [],
      datasets: [
        { label: "Circuit 1 (W)", borderColor: "#0288d1", backgroundColor: "rgba(2, 136, 209, 0.08)", data: [], fill: true, tension: 0.2 },
        { label: "Circuit 2 (W)", borderColor: "#e65100", backgroundColor: "rgba(230, 81, 0, 0.08)", data: [], fill: true, tension: 0.2 }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      scales: { x: { display: true }, y: { beginAtZero: true } }
    }
  });

  function updatePower() {
    fetch("/api/data")
      .then((res) => {
        if (!res.ok) throw new Error("Erreur");
        return res.json();
      })
      .then((data) => {
        p1El.innerText = data.p1.toFixed(2);
        i1El.innerText = data.i1.toFixed(2);
        p2El.innerText = data.p2.toFixed(2);
        i2El.innerText = data.i2.toFixed(2);
        vEl.innerText = data.v.toFixed(1);

        p1El.style.color = data.p1 < 0 ? "#2e7d32" : "#0288d1";
        p2El.style.color = data.p2 < 0 ? "#2e7d32" : "#e65100";

        if (data.opto) {
          optoBadge.innerText = "ACTIF (Injection)";
          optoBadge.style.backgroundColor = "#e8f5e9";
          optoBadge.style.color = "#2e7d32";
        } else {
          optoBadge.innerText = "INACTIF";
          optoBadge.style.backgroundColor = "#eceff1";
          optoBadge.style.color = "#546e7a";
        }

        statusEl.innerText = "En direct";
        statusEl.classList.remove("error");
      })
      .catch(() => {
        statusEl.innerText = "Hors ligne";
        statusEl.classList.add("error");
      });
  }

  function updateChart() {
    const period = periodSelect.value;
    fetch(`/api/history?period=${period}`)
      .then((res) => res.json())
      .then((data) => {
        const labels = data.ts.map((ts) => {
          const d = new Date(ts * 1000);
          if (period === "12h") return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
          if (period === "7d") return `${d.toLocaleDateString([], { weekday: "short" })} ${d.getHours()}h`;
          return d.toLocaleDateString([], { month: "short", day: "numeric" });
        });

        powerChart.data.labels = labels;
        powerChart.data.datasets[0].data = data.p1;
        powerChart.data.datasets[1].data = data.p2;
        powerChart.update();
      })
      .catch(() => {});
  }

  function loadLittleFsFiles() {
    fetch("/api/files")
      .then((res) => res.json())
      .then((files) => {
        littlefsFiles.innerHTML = files
          .map((f) => `<li>• <strong>${f.name}</strong> (${(f.size / 1024).toFixed(1)} Ko)</li>`)
          .join("");
      })
      .catch(() => {
        littlefsFiles.innerHTML = "<li>Impossible de lister les fichiers</li>";
      });
  }

  if (btnUploadWeb) {
    btnUploadWeb.addEventListener("click", async () => {
      if (!webFilesInput.files.length) {
        uploadStatus.innerText = "Veuillez sélectionner au moins un fichier.";
        uploadStatus.style.color = "#c62828";
        return;
      }

      btnUploadWeb.disabled = true;
      for (const file of webFilesInput.files) {
        uploadStatus.innerText = `Envoi de ${file.name}...`;
        uploadStatus.style.color = "#0288d1";

        try {
          const res = await fetch(`/upload?file=${encodeURIComponent(file.name)}`, {
            method: "POST",
            body: file
          });
          if (!res.ok) throw new Error();
        } catch {
          uploadStatus.innerText = `Erreur lors de l'envoi de ${file.name}.`;
          uploadStatus.style.color = "#c62828";
          btnUploadWeb.disabled = false;
          return;
        }
      }

      uploadStatus.innerText = "Mise à jour réussie ! Actualisez la page.";
      uploadStatus.style.color = "#2e7d32";
      btnUploadWeb.disabled = false;
      webFilesInput.value = "";
      loadLittleFsFiles();
    });

    loadLittleFsFiles();
  }

  if (btnUploadOta) {
    btnUploadOta.addEventListener("click", () => {
      const file = otaFileInput.files[0];
      if (!file || !file.name.endsWith(".bin")) {
        otaStatus.innerText = "Veuillez sélectionner un fichier .bin valide.";
        otaStatus.style.color = "#c62828";
        return;
      }

      btnUploadOta.disabled = true;
      otaStatus.style.color = "#0288d1";

      const xhr = new XMLHttpRequest();
      xhr.open("POST", "/ota", true);

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const pct = Math.round((e.loaded / e.total) * 100);
          otaStatus.innerText = `Téléversement en cours : ${pct}%...`;
        }
      };

      xhr.onload = () => {
        if (xhr.status === 200) {
          otaStatus.innerText = "Firmware installé ! Redémarrage de l'ESP32...";
          otaStatus.style.color = "#2e7d32";
          setTimeout(() => location.reload(), 6000);
        } else {
          otaStatus.innerText = `Erreur OTA : ${xhr.responseText || xhr.status}`;
          otaStatus.style.color = "#c62828";
          btnUploadOta.disabled = false;
        }
      };

      xhr.onerror = () => {
        otaStatus.innerText = "Erreur réseau pendant le transfert.";
        otaStatus.style.color = "#c62828";
        btnUploadOta.disabled = false;
      };

      xhr.send(file);
    });
  }

  fetch("/api/config")
    .then((res) => res.json())
    .then((cfg) => {
      if (cfg.mqtt_uri) document.getElementById("mqtt_uri").value = cfg.mqtt_uri;
      if (cfg.mqtt_topic) document.getElementById("mqtt_topic").value = cfg.mqtt_topic;
      if (cfg.th_on !== undefined) document.getElementById("th_on").value = cfg.th_on;
      if (cfg.th_off !== undefined) document.getElementById("th_off").value = cfg.th_off;
    })
    .catch(() => {});

  periodSelect.addEventListener("change", updateChart);

  setInterval(updatePower, 1000);
  setInterval(updateChart, 15000);
  updatePower();
  updateChart();
});
