// ==========================================
// PLANT HUB PRO - OCEAN GLASS DASHBOARD
// ==========================================

var moistureChart, tempChart, humChart, pressureChart;
var maxPoints = 50;
var tempFahrenheit = true;
var pressureInHg = false;

// ===== TOGGLES =====
function toggleTemp() {
  tempFahrenheit = document.getElementById('tempToggle').checked;
  tempChart.options.plugins.title.text = tempFahrenheit ? 'Temp F' : 'Temp C';
  tempChart.update('none');
}
function togglePressure() {
  pressureInHg = document.getElementById('pressToggle').checked;
  pressureChart.options.plugins.title.text = pressureInHg ? 'Pressure inHg' : 'Pressure hPa';
  pressureChart.update('none');
}

// ===== CHARTS =====
function initCharts() {
  var opts = function(label, color) {
    return {
      type: 'line',
      data: {
        labels: [],
        datasets: [{
          label: label,
          data: [],
          borderColor: color,
          backgroundColor: color.replace('1)', '0.15)'),
          fill: true,
          tension: 0.4,
          borderWidth: 2,
          pointRadius: 0
        }]
      },
      options: {
        animation: { duration: 400 },
        responsive: true,
        maintainAspectRatio: false,
        plugins: {
          legend: { display: false },
          title: { display: true, text: label, font: { size: 11, family: 'DM Sans' }, color: 'rgba(255,255,255,0.5)' }
        },
        scales: {
          x: { display: false },
          y: {
            beginAtZero: false,
            grid: { color: 'rgba(255,255,255,0.05)' },
            ticks: { font: { size: 10 }, color: 'rgba(255,255,255,0.3)' }
          }
        }
      }
    };
  };

  moistureChart = new Chart(document.getElementById('moistureChart'), opts('Moisture', 'rgba(34,211,238,1)'));
  tempChart = new Chart(document.getElementById('tempChart'), opts('Temp F', 'rgba(251,191,36,1)'));
  humChart = new Chart(document.getElementById('humChart'), opts('Humidity', 'rgba(96,165,250,1)'));
  pressureChart = new Chart(document.getElementById('pressureChart'), opts('Pressure hPa', 'rgba(74,222,128,1)'));
}

function addChartData(chart, label, value) {
  chart.data.labels.push(label);
  chart.data.datasets[0].data.push(value);
  if (chart.data.labels.length > maxPoints) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
  }
  var d = chart.data.datasets[0].data;
  chart.options.scales.y.min = Math.min.apply(null, d) * 0.95;
  chart.options.scales.y.max = Math.max.apply(null, d) * 1.05;
  chart.update('none');
}

// ===== HELPERS =====
function pctClass(pct) {
  return pct < 40 ? 'dry' : pct < 70 ? 'good' : 'wet';
}
function badgeClass(pct) {
  return pct < 40 ? 'badge-dry' : pct < 70 ? 'badge-good' : 'badge-wet';
}
function statusLabel(pct) {
  return pct < 40 ? 'DRY' : pct < 70 ? 'OK' : 'WET';
}
function formatTime(s) {
  if (typeof s !== 'number') return '--';
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.floor(s / 60) + 'm ' + (s % 60) + 's ago';
  return Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm ago';
}
function formatUptime(m) {
  if (m < 60) return m + 'm';
  if (m < 1440) return Math.floor(m / 60) + 'h ' + (m % 60) + 'm';
  return Math.floor(m / 1440) + 'd ' + Math.floor((m % 1440) / 60) + 'h';
}

// ===== BUILD ZONE CARDS =====
function buildZoneCards(zones) {
  var grid = document.getElementById('zonesGrid');
  grid.innerHTML = '';

  var locals = [];
  var remotes = [];
  for (var i = 0; i < zones.length; i++) {
    if (zones[i].node === 'NODE0') locals.push(zones[i]);
    else remotes.push(zones[i]);
  }

  // Local zones
  for (var j = 0; j < locals.length; j++) {
    var z = locals[j];
    var card = document.createElement('div');
    card.className = 'zone-card';
    var isWatering = z.watering === true;
    card.innerHTML =
      '<div class="zone-name">Zone ' + z.zone + '</div>' +
      '<div class="zone-pct ' + pctClass(z.moisture) + '">' + z.moisture + '%</div>' +
      '<span class="zone-badge ' + badgeClass(z.moisture) + '">' + statusLabel(z.moisture) + '</span>' +
      '<div class="zone-details">' + z.volts.toFixed(3) + 'V</div>' +
      '<button class="zone-water-btn' + (isWatering ? ' watering' : '') + '" onclick="waterZone(' + z.zone + ')">' +
        (isWatering ? 'WATERING...' : 'Manual Water') +
      '</button>';
    grid.appendChild(card);
  }

  // Remote section
  var remoteSection = document.getElementById('remoteSection');
  var remoteGrid = document.getElementById('remoteGrid');

  if (remotes.length === 0) {
    remoteSection.style.display = 'none';
    return;
  }

  remoteSection.style.display = '';
  remoteGrid.innerHTML = '';

  for (var r = 0; r < remotes.length; r++) {
    var n = remotes[r];
    var lastSeen = n.lastSeen !== undefined ? n.lastSeen : null;
    var batt = n.battery !== undefined ? n.battery.toFixed(2) + 'V' : '--';
    var stale = (typeof lastSeen === 'number' && lastSeen > 1860);

    var rcard = document.createElement('div');
    rcard.className = 'zone-card remote';
    rcard.innerHTML =
      '<div class="zone-name">' + n.node + ' Zone ' + n.zone + '</div>' +
      '<div class="zone-pct ' + pctClass(n.moisture) + '">' + n.moisture + '%</div>' +
      '<span class="zone-badge ' + badgeClass(n.moisture) + '">' + statusLabel(n.moisture) + '</span>' +
      '<div class="zone-details">' +
        'Batt: ' + batt + '<br>' +
        '<span class="' + (stale ? 'stale' : '') + '">Last: ' + formatTime(lastSeen) + '</span>' +
      '</div>';
    remoteGrid.appendChild(rcard);
  }
}

// ===== FETCH & UPDATE =====
function fetchData() {
  fetch('/data.json')
    .then(function(res) { return res.json(); })
    .then(function(data) {

      // Header stats
      document.getElementById('heapBar').innerText = (data.heap / 1024).toFixed(0) + ' KB';
      document.getElementById('uptimeBar').innerText = formatUptime(data.uptime);
      document.getElementById('ipBar').innerText = data.ip;
      document.getElementById('hubDot').className = 'hstat-dot online';

      // Environment
      var tf = tempFahrenheit ? (data.temp * 1.8 + 32).toFixed(1) + ' F' : data.temp.toFixed(1) + ' C';
      document.getElementById('envTemp').innerText = tf;
      document.getElementById('envHum').innerText = data.hum.toFixed(1) + '%';
      var pf = pressureInHg ? (data.press / 33.8639).toFixed(2) + '"Hg' : data.press.toFixed(0) + ' hPa';
      document.getElementById('envPress').innerText = pf;
      document.getElementById('envVPD').innerText = data.vpd.toFixed(2) + ' kPa';
      document.getElementById('envBatt').innerText = data.battery.toFixed(2) + ' V';

      // Build zone cards
      if (data.zones) {
        buildZoneCards(data.zones);
      }

      // Weather
      if (data.weather && data.weather.valid) {
        document.getElementById('weatherSection').style.display = '';
        document.getElementById('wxForecast').innerText = data.weather.forecast;
        document.getElementById('wxTemp').innerText = data.weather.tempF.toFixed(0) + ' F';
        document.getElementById('wxRain').innerText = data.weather.rainChance + '%';
        document.getElementById('wxRain6h').innerText = data.weather.rain6h ? 'YES' : 'No';
      } else {
        document.getElementById('weatherSection').style.display = 'none';
      }

      // Charts - use zone 0 for primary
      var zone0 = null;
      if (data.zones) {
        for (var i = 0; i < data.zones.length; i++) {
          if (data.zones[i].node === 'NODE0' && data.zones[i].zone === 0) {
            zone0 = data.zones[i];
            break;
          }
        }
      }

      if (zone0) {
        addChartData(moistureChart, data.time, zone0.moisture);
        addChartData(tempChart, data.time, tempFahrenheit ? (data.temp * 1.8 + 32) : data.temp);
        addChartData(humChart, data.time, data.hum);
        addChartData(pressureChart, data.time, pressureInHg ? (data.press / 33.8639) : data.press);

        // Alert if dry
        if (zone0.moisture < 40) {
          flashAlert('Zone 0 is dry! ' + zone0.moisture + '%');
        }
      }

    })
    .catch(function(err) {
      console.error('Fetch error:', err);
      document.getElementById('hubDot').className = 'hstat-dot';
    });
}

// ===== COMMANDS =====
var API_KEY = 'planthub2026';

function sendCommand(cmd) {
  fetch('/' + cmd + '?key=' + API_KEY).catch(function(e) { console.error('Command failed:', e); });
}

function waterZone(zone) {
  sendCommand('waterZone' + zone);
}

// ===== ALERT =====
var flashTimer;
function flashAlert(msg) {
  var banner = document.getElementById('alertBanner');
  banner.innerText = msg;
  banner.classList.remove('hidden');
  clearTimeout(flashTimer);
  flashTimer = setTimeout(function() {
    banner.classList.add('hidden');
  }, 4000);
}

// ===== INIT =====
window.onload = function() {
  document.getElementById('alertBanner').classList.add('hidden');
  initCharts();
  fetchData();
  setInterval(fetchData, 2000);
};
