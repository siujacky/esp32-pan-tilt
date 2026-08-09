const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<link rel="icon" href="data:,">
<title>ESP32 Pan / Tilt</title>
<style>
  *{box-sizing:border-box;}
  :root{
    --bg:#0d1117; --panel:#161b22; --btn:#21262d; --edge:#30363d;
    --txt:#e6edf3; --muted:#8b949e; --accent:#2f81f7; --accent2:#1f6feb;
    --home:#d29922; --home-ink:#1a1200; --bad:#f85149;
    --size:76px; --gap:12px;
  }
  html,body{height:100%;}
  body{
    margin:0; background:var(--bg); color:var(--txt);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
    -webkit-font-smoothing:antialiased;
    display:flex; flex-direction:column; align-items:center;
    min-height:100vh; padding:26px 16px 44px; gap:20px;
    overscroll-behavior:none; -webkit-tap-highlight-color:transparent;
    user-select:none; -webkit-user-select:none;
  }
  header{text-align:center;}
  h1{margin:0 0 12px; font-size:20px; font-weight:600; letter-spacing:.3px;}
  .badge{
    display:inline-block; font-size:13px; font-weight:600;
    padding:5px 13px; border-radius:999px; background:var(--panel);
    border:1px solid var(--edge); color:var(--muted);
    font-variant-numeric:tabular-nums;
  }
  button.badge{-webkit-appearance:none; appearance:none; cursor:pointer; font-family:inherit;}
  button.badge::after{content:" \2699"; opacity:.7;}   /* gear = tap for settings */
  body.offline .badge{color:var(--bad); border-color:var(--bad);}
  .readout{
    font-size:17px; font-weight:600; background:var(--panel);
    border:1px solid var(--edge); border-radius:12px; padding:12px 22px;
    font-variant-numeric:tabular-nums; letter-spacing:.2px; text-align:center;
  }
  .readout b{color:var(--accent); font-weight:700;}
  .homing{
    display:inline-block; margin-left:8px; font-size:13px; font-weight:700;
    color:var(--home); vertical-align:middle;
  }
  .homing[hidden]{display:none;}
  .tabs{display:flex; gap:6px; background:var(--panel); border:1px solid var(--edge); border-radius:12px; padding:4px;}
  .tabs[hidden]{display:none;}   /* display:flex would otherwise defeat the hidden attribute */
  .tab{-webkit-appearance:none; appearance:none; cursor:pointer; font-family:inherit; font-size:13px; font-weight:600;
       padding:8px 16px; border-radius:9px; border:1px solid transparent; background:transparent; color:var(--muted);}
  .tab.active{background:var(--accent2); border-color:var(--accent); color:#fff;}
  .tab:disabled{opacity:.4; cursor:not-allowed;}
  .dpad{
    display:grid; grid-template-columns:repeat(3,var(--size));
    grid-template-rows:repeat(3,var(--size)); gap:var(--gap);
  }
  .pad{
    -webkit-appearance:none; appearance:none; margin:0; padding:0;
    border:1px solid var(--edge); background:var(--btn); color:var(--txt);
    border-radius:16px; font-size:30px; line-height:1; font-family:inherit;
    display:flex; align-items:center; justify-content:center; cursor:pointer;
    touch-action:none; user-select:none; -webkit-user-select:none;
    -webkit-touch-callout:none;
    transition:background .06s ease, transform .06s ease, border-color .06s ease;
  }
  .pad:active{transform:translateY(1px);}
  .dir:active{background:var(--accent2); border-color:var(--accent); color:#fff;}
  .up{grid-area:1/2;} .left{grid-area:2/1;} .home{grid-area:2/2;}
  .right{grid-area:2/3;} .down{grid-area:3/2;}
  .home{background:transparent; border-color:var(--home); color:var(--home); font-size:26px;}
  .home:active{background:var(--home); border-color:var(--home); color:var(--home-ink);}
  /* shared card width for the slider + advanced panels */
  .card{
    width:100%; max-width:calc(var(--size)*3 + var(--gap)*2);
    background:var(--panel); border:1px solid var(--edge);
    border-radius:14px; padding:14px 16px;
  }
  .speed-row{display:flex; justify-content:space-between; align-items:baseline; margin-bottom:12px;}
  .speed-row label{font-size:14px; font-weight:600; color:var(--muted);}
  .step-val{font-size:16px; font-weight:700; color:var(--accent); font-variant-numeric:tabular-nums;}
  input[type=range]{
    -webkit-appearance:none; appearance:none; width:100%; height:6px;
    border-radius:999px; background:var(--btn); outline:none; margin:0;
  }
  input[type=range]::-webkit-slider-thumb{
    -webkit-appearance:none; appearance:none; width:26px; height:26px;
    border-radius:50%; background:var(--accent); border:3px solid var(--bg); cursor:pointer;
  }
  input[type=range]::-moz-range-thumb{
    width:26px; height:26px; border-radius:50%; background:var(--accent);
    border:none; cursor:pointer;
  }
  .panel-h{
    display:flex; justify-content:space-between; align-items:baseline; gap:8px;
    font-size:14px; font-weight:700; color:var(--muted); margin-bottom:12px;
  }
  .panel-h .sum{
    font-size:12px; font-weight:600; color:var(--accent);
    font-variant-numeric:tabular-nums; letter-spacing:.2px;
  }
  .grid{display:grid; grid-template-columns:1fr 1fr; gap:12px;}
  .cell{display:flex; flex-direction:column; gap:6px;}
  .cell label{font-size:12px; font-weight:600; color:var(--muted);}
  input[type=number],input[type=password],select{
    -webkit-appearance:none; appearance:none; width:100%; margin:0;
    background:var(--btn); border:1px solid var(--edge); color:var(--txt);
    border-radius:9px; padding:9px 10px; font-size:16px; font-family:inherit;
    font-variant-numeric:tabular-nums;
  }
  input[type=number]{text-align:center;}
  input[type=number]::-webkit-outer-spin-button,
  input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none; margin:0;}
  .act{
    -webkit-appearance:none; appearance:none; cursor:pointer; font-family:inherit;
    background:var(--btn); border:1px solid var(--edge); color:var(--txt);
    border-radius:9px; padding:8px 6px; font-size:12px; font-weight:600;
    transition:background .06s ease, border-color .06s ease;
  }
  .act:active{background:var(--accent2); border-color:var(--accent); color:#fff;}
  .act:disabled{opacity:.5;}
  .act.wide{width:100%; margin-bottom:12px; padding:11px; font-size:13px;}
  .hs{margin-top:14px;}
  .reset{
    -webkit-appearance:none; appearance:none; cursor:pointer; font-family:inherit;
    background:transparent; border:1px solid var(--edge); color:var(--muted);
    border-radius:10px; padding:9px 16px; font-size:12px; font-weight:600;
  }
  .reset:active{border-color:var(--bad); color:var(--bad);}
  .wmsg{font-size:12px; color:var(--muted); margin-top:10px; line-height:1.4;}
  /* advanced settings, revealed by tapping the badge */
  .adv{
    display:flex; flex-direction:column; align-items:center; gap:20px;
    width:100%; max-width:calc(var(--size)*3 + var(--gap)*2);
  }
  .adv[hidden]{display:none;}
  .conn{
    font-size:13px; font-weight:600; color:var(--bad);
    background:rgba(248,81,73,.12); border:1px solid var(--bad);
    padding:9px 15px; border-radius:10px; text-align:center;
  }
  .conn[hidden]{display:none;}
  /* ---- LX-16A serial-servo cards (advanced panel; LX cards shown only when bknd==1) ---- */
  .card[hidden]{display:none;}
  .row{display:flex; gap:8px;}
  .row>input,.row>select{flex:1 1 0; min-width:0;}
  .row>.act{flex:0 0 auto; white-space:nowrap;}
  .tel{
    font-size:14px; font-weight:600; color:var(--txt);
    font-variant-numeric:tabular-nums; padding:3px 0; line-height:1.4;
  }
  .tel .warn{color:var(--bad); font-weight:700;}
  .lxout{
    font-size:12px; color:var(--muted); margin-top:10px;
    line-height:1.6; font-variant-numeric:tabular-nums; word-break:break-word;
  }
  .lxout div{color:var(--txt);}
  .chk{display:flex; align-items:center; gap:9px; font-size:12px; color:var(--muted); margin:0 0 12px;}
  .chk input[type=checkbox]{width:18px; height:18px; margin:0; accent-color:var(--accent); flex:0 0 auto;}
  .cfg-row{margin-top:12px;}
  .cfg-row>label{display:block; font-size:12px; font-weight:600; color:var(--muted); margin-bottom:6px;}
</style>
</head>
<body>
  <header>
    <h1>ESP32 Pan / Tilt</h1>
    <button type="button" id="badge" class="badge" title="Tap for advanced settings">connecting&hellip;</button>
  </header>

  <div class="readout">Pan <b id="pan">90</b>&deg; / Tilt <b id="tilt">90</b>&deg;<span id="homing" class="homing" hidden>&#8226; homing&hellip;</span></div>

  <!-- Serial/Both start disabled: before the first status poll the page doesn't know the
       backend, and a click in PWM mode is silently ignored by the firmware; the poll's
       setTabs() enables them when the LX backend is live. -->
  <div class="tabs" id="tabs" title="Which servos the D-pad drives">
    <button type="button" class="tab active" data-t="0">PWM</button>
    <button type="button" class="tab" data-t="1" disabled>Serial</button>
    <button type="button" class="tab" data-t="2" disabled>Both</button>
  </div>

  <div class="dpad">
    <button type="button" class="pad dir up"    data-cmd="B,U" aria-label="Tilt up">&#9650;</button>
    <button type="button" class="pad dir left"  data-cmd="B,L" aria-label="Pan left">&#9664;</button>
    <button type="button" class="pad home" id="home"           aria-label="Home (glide to home)">&#8962;</button>
    <button type="button" class="pad dir right" data-cmd="B,R" aria-label="Pan right">&#9654;</button>
    <button type="button" class="pad dir down"  data-cmd="B,D" aria-label="Tilt down">&#9660;</button>
  </div>

  <div class="card">
    <div class="speed-row">
      <label for="step">Speed / Step</label>
      <span id="stepVal" class="step-val">5&deg;</span>
    </div>
    <input type="range" id="step" min="1" max="30" value="5" aria-label="Step size in degrees">
  </div>

  <div id="adv" class="adv" hidden>

    <!-- Config groups as tabs (user request): find settings without scrolling a long stack. -->
    <div class="tabs" id="cfgTabs" style="margin-bottom:12px;">
      <button type="button" class="tab active" data-g="rig">Rig</button>
      <button type="button" class="tab" data-g="lx" id="cfgTabLx" hidden>Serial</button>
      <button type="button" class="tab" data-g="pwm">Digital</button>
      <button type="button" class="tab" data-g="joy">Joystick</button>
      <button type="button" class="tab" data-g="sys">System</button>
    </div>

    <div class="card" data-g="sys">
      <div class="panel-h"><span>WiFi setup</span><span id="wCur" class="sum"></span></div>
      <button type="button" class="act wide" id="wScan">Scan networks</button>
      <div class="cell" style="margin-bottom:12px;">
        <label for="wSsid">Network</label>
        <select id="wSsid"><option value="">&mdash; scan first &mdash;</option></select>
      </div>
      <div class="cell" style="margin-bottom:12px;">
        <label for="wPass">Password</label>
        <input type="password" id="wPass" placeholder="blank if open" autocomplete="off">
      </div>
      <button type="button" class="act wide" id="wJoin">Save &amp; join network</button>
      <button type="button" class="reset" id="wForget">Forget &mdash; hotspot only</button>
      <div id="wMsg" class="wmsg"></div>
    </div>

    <div class="card" data-g="sys">
      <div class="panel-h"><span>Servo backend</span><span id="bkndCur" class="sum"></span></div>
      <button type="button" class="act wide" id="bkndToggle">Switch backend</button>
      <div id="bkndMsg" class="wmsg">PWM drives SG90 servos on GPIO&nbsp;25/26. LX-16A uses a one-wire serial bus on the pin below. Switching backend reboots the board.</div>
      <div class="cfg-row">
        <label for="lxPin">LX-16A bus pin (GPIO)</label>
        <div class="row">
          <input type="number" id="lxPin" min="0" max="33" inputmode="numeric" aria-label="LX-16A bus GPIO">
          <button type="button" class="act" id="lxPinSave">Save &amp; reboot</button>
        </div>
        <div id="lxPinMsg" class="wmsg">Saved to flash and applied on the next boot. Usable: 4, 5, 13, 14, 17, 18, 19, 23, 27, 32, 33 &mdash; the board rejects input-only, flash, console, strapping and already-used pins.</div>
      </div>
    </div>

    <div class="card" data-g="pwm">
      <div class="panel-h"><span>Digital servo (PWM / SG90)</span><span id="pwmSum" class="sum"></span></div>
      <div class="grid">
        <div class="cell">
          <label for="pwmPanSel">Pan pin</label>
          <select id="pwmPanSel" aria-label="PWM pan GPIO"></select>
        </div>
        <div class="cell">
          <label for="pwmTiltSel">Tilt pin</label>
          <select id="pwmTiltSel" aria-label="PWM tilt GPIO"></select>
        </div>
      </div>
      <button type="button" class="act wide" id="pwmPinSave" style="margin-top:12px;">Save pins &amp; reboot</button>
      <div id="pwmPinMsg" class="wmsg">Pins already used by the serial bus, I2C, LEDs, or the other servo are not listed. Saved to flash and applied on the next boot.</div>
    </div>

    <div class="card" data-g="joy">
      <div class="panel-h"><span>Joystick</span><span id="joySum" class="sum"></span></div>
      <div class="grid">
        <div class="cell">
          <label for="joyEn">Stick input</label>
          <select id="joyEn"><option value="1">Enabled</option><option value="0">Disabled</option></select>
        </div>
        <div class="cell">
          <label for="joyMd">Drive mode</label>
          <select id="joyMd"><option value="0">RATE &mdash; hold to jog</option><option value="1">ABSOLUTE &mdash; glide to stick</option></select>
        </div>
      </div>
      <div class="cfg-row">
        <label>Button map</label>
        <div class="grid">
          <div class="cell"><label for="kbHm">Home</label><select id="kbHm"></select></div>
          <div class="cell"><label for="kbBk">Back</label><select id="kbBk"></select></div>
          <div class="cell"><label for="kbEn">Confirm / edit</label><select id="kbEn"></select></div>
          <div class="cell"><label for="kbTg">Drive &#8646; Menu</label><select id="kbTg"></select></div>
        </div>
        <div id="kbWarn" class="wmsg"></div>
      </div>
      <div class="wmsg">Buttons are remapped here only &mdash; never from the stick, so a bad map can't lock you out of the on-device menu. The centre-click (OK) is stiff; the defaults avoid it.</div>
    </div>

    <div class="tabs lxcard" data-g="lx" id="svTabs" hidden style="align-self:stretch;">
      <button type="button" class="tab active" data-sv="main">Main</button>
      <button type="button" class="tab" data-sv="ids">IDs</button>
      <button type="button" class="tab" data-sv="det">Detail</button>
    </div>

    <div class="card lxcard" data-g="lx" data-sv="main" hidden>
      <div class="panel-h"><span>Servo telemetry</span><span id="telFault" class="sum"></span></div>
      <div class="grid">
        <div class="cell"><label>Pan servo</label><div id="telPan" class="tel">&mdash;</div></div>
        <div class="cell"><label>Tilt servo</label><div id="telTilt" class="tel">&mdash;</div></div>
      </div>
      <div class="cell" style="margin-top:10px;">
        <label for="lxRelaxSel">Torque when parked</label>
        <select id="lxRelaxSel">
          <option value="0">Hold position (buzzes if it can't quite reach)</option>
          <option value="1">Release after 3&nbsp;s &mdash; silent, but no holding force</option>
        </select>
      </div>
    </div>

    <div class="card lxcard" data-g="lx" data-sv="ids" hidden>
      <div class="panel-h"><span>Bus scan</span><span id="scanSum" class="sum"></span></div>
      <div class="row">
        <input type="number" id="scanHi" min="1" max="253" value="30" inputmode="numeric" aria-label="Range scan highest ID">
        <button type="button" class="act" id="scanRange">Scan range</button>
        <button type="button" class="act" id="scanLone">Lone</button>
      </div>
      <div id="scanOut" class="lxout"></div>
    </div>

    <div class="card lxcard" data-g="lx" data-sv="ids" hidden>
      <div class="panel-h"><span>Link servo IDs</span></div>
      <div class="grid">
        <div class="cell">
          <label for="panSel">Pan servo ID</label>
          <select id="panSel"></select>
        </div>
        <div class="cell">
          <label for="tiltSel">Tilt servo ID</label>
          <select id="tiltSel"></select>
        </div>
      </div>
      <div id="idWarn" class="wmsg"></div>
    </div>

    <div class="card lxcard" data-g="lx" data-sv="det" hidden>
      <div class="panel-h"><span>Servo config</span></div>
      <div class="row">
        <select id="cfgId" aria-label="Config servo ID"></select>
        <button type="button" class="act" id="cfgRead">Read all</button>
      </div>

      <div class="cfg-row">
        <label>Angle limits (ticks 0&#8211;1000)</label>
        <div class="row">
          <input type="number" id="cfgAlimMin" min="0" max="1000" inputmode="numeric" aria-label="Angle limit min ticks">
          <input type="number" id="cfgAlimMax" min="0" max="1000" inputmode="numeric" aria-label="Angle limit max ticks">
          <button type="button" class="act" id="cfgSetAlim">Set</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>Voltage limits (mV 4500&#8211;12000)</label>
        <div class="row">
          <input type="number" id="cfgVlimMin" min="4500" max="12000" inputmode="numeric" aria-label="Voltage limit min mV">
          <input type="number" id="cfgVlimMax" min="4500" max="12000" inputmode="numeric" aria-label="Voltage limit max mV">
          <button type="button" class="act" id="cfgSetVlim">Set</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>Temperature cap (&deg;C 50&#8211;100)</label>
        <div class="row">
          <input type="number" id="cfgTlim" min="50" max="100" inputmode="numeric" aria-label="Temperature cap C">
          <button type="button" class="act" id="cfgSetTlim">Set</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>Mode &amp; motor speed (&minus;1000&#8230;1000)</label>
        <div class="row">
          <select id="cfgMode" aria-label="Servo or motor mode">
            <option value="0">servo</option>
            <option value="1">motor</option>
          </select>
          <input type="number" id="cfgSpeed" min="-1000" max="1000" value="0" inputmode="numeric" aria-label="Motor speed">
          <button type="button" class="act" id="cfgSetMode">Set</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>Torque</label>
        <div class="row">
          <select id="cfgTorque" aria-label="Torque load or unload">
            <option value="1">load (hold)</option>
            <option value="0">unload (free)</option>
          </select>
          <button type="button" class="act" id="cfgSetTorque">Set</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>Angle trim (&minus;125&#8230;125)</label>
        <div class="row">
          <input type="number" id="cfgTrim" min="-125" max="125" inputmode="numeric" aria-label="Angle trim">
          <button type="button" class="act" id="cfgSetTrim">Set</button>
          <button type="button" class="act" id="cfgSaveTrim">Save</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>LED (inverted: on / off)</label>
        <div class="row">
          <select id="cfgLed" aria-label="LED on or off">
            <option value="0">on</option>
            <option value="1">off</option>
          </select>
          <button type="button" class="act" id="cfgSetLed">Set</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>LED alarm mask (0&#8211;7: 1 temp, 2 volt, 4 stall)</label>
        <div class="row">
          <input type="number" id="cfgLederr" min="0" max="7" inputmode="numeric" aria-label="LED alarm mask">
          <button type="button" class="act" id="cfgSetLederr">Set</button>
        </div>
      </div>

      <button type="button" class="reset" id="cfgFactory" style="margin-top:14px;">Factory-reset servo limits</button>
      <div id="cfgMsg" class="wmsg"></div>
    </div>

    <div class="card lxcard" data-g="lx" data-sv="ids" hidden>
      <div class="panel-h"><span>Program servo ID</span><span class="sum">one at a time</span></div>

      <label class="chk"><input type="checkbox" id="progOne">Exactly one servo is connected to the bus</label>
      <div class="cfg-row" style="margin-top:0;">
        <label>Broadcast-program the lone servo</label>
        <div class="row">
          <input type="number" id="progNew" min="0" max="253" placeholder="new ID" inputmode="numeric" aria-label="New ID (broadcast)">
          <button type="button" class="act" id="progBroadcast">Program</button>
        </div>
      </div>

      <div class="cfg-row">
        <label>Targeted re-ID (duplicate-checked)</label>
        <div class="row">
          <input type="number" id="progCur" min="0" max="253" placeholder="current" inputmode="numeric" aria-label="Current ID">
          <input type="number" id="progNew2" min="0" max="253" placeholder="new" inputmode="numeric" aria-label="New ID (targeted)">
          <button type="button" class="act" id="progTargeted">Re-ID</button>
        </div>
      </div>
      <div id="progMsg" class="wmsg"></div>
    </div>

    <div class="card" data-g="rig">
      <div class="panel-h"><span>Soft limits</span><span id="limSum" class="sum">Pan 0&#8211;180 &#183; Tilt 0&#8211;180</span></div>
      <div class="grid">
        <div class="cell">
          <label for="pmin">Pan min</label>
          <input type="number" id="pmin" min="0" max="180" value="0" data-set="N,PL" inputmode="numeric" aria-label="Pan minimum angle">
          <button type="button" class="act" data-cmd="C,PL">Set = current</button>
        </div>
        <div class="cell">
          <label for="pmax">Pan max</label>
          <input type="number" id="pmax" min="0" max="180" value="180" data-set="N,PH" inputmode="numeric" aria-label="Pan maximum angle">
          <button type="button" class="act" data-cmd="C,PH">Set = current</button>
        </div>
        <div class="cell">
          <label for="tmin">Tilt min</label>
          <input type="number" id="tmin" min="0" max="180" value="0" data-set="N,TL" inputmode="numeric" aria-label="Tilt minimum angle">
          <button type="button" class="act" data-cmd="C,TL">Set = current</button>
        </div>
        <div class="cell">
          <label for="tmax">Tilt max</label>
          <input type="number" id="tmax" min="0" max="180" value="180" data-set="N,TH" inputmode="numeric" aria-label="Tilt maximum angle">
          <button type="button" class="act" data-cmd="C,TH">Set = current</button>
        </div>
      </div>
    </div>

    <div class="card" data-g="rig">
      <div class="panel-h"><span>Home</span></div>
      <button type="button" class="act wide" data-cmd="C,SH">Set Home = current</button>
      <div class="grid">
        <div class="cell">
          <label for="hpan">Home pan</label>
          <input type="number" id="hpan" min="0" max="180" value="90" data-set="N,HP" inputmode="numeric" aria-label="Home pan angle">
        </div>
        <div class="cell">
          <label for="htilt">Home tilt</label>
          <input type="number" id="htilt" min="0" max="180" value="90" data-set="N,HT" inputmode="numeric" aria-label="Home tilt angle">
        </div>
      </div>
      <div class="speed-row hs">
        <label for="hspd">Home speed</label>
        <span id="hspdVal" class="step-val">90&deg;/s</span>
      </div>
      <input type="range" id="hspd" min="10" max="300" value="90" aria-label="Home glide speed, degrees per second">
    </div>

    <button type="button" id="reset" class="reset" data-g="rig">Reset calibration</button>

  </div><!-- /#adv -->

  <div id="conn" class="conn" hidden>&#9888; disconnected &mdash; check power / WiFi</div>

<script>
(function(){
  var REPEAT_MS = 120;                                  // auto-repeat cadence for held direction pads
  var POLL_MS   = 700;                                  // background /status poll interval
  var $ = function(id){ return document.getElementById(id); };
  var badge = $('badge'), panEl = $('pan'), tiltEl = $('tilt'), homingEl = $('homing');
  var slider = $('step'), stepVal = $('stepVal');
  var pmin = $('pmin'), pmax = $('pmax'), tmin = $('tmin'), tmax = $('tmax');
  var hpan = $('hpan'), htilt = $('htilt'), hspd = $('hspd'), hspdVal = $('hspdVal');
  var limSum = $('limSum'), reset = $('reset'), conn = $('conn'), home = $('home');
  var adv = $('adv');
  var wScan = $('wScan'), wSsid = $('wSsid'), wPass = $('wPass'), wJoin = $('wJoin'),
      wForget = $('wForget'), wMsg = $('wMsg'), wCur = $('wCur');
  var timer = null;
  var busy  = false;                                    // true while an /action fetch is outstanding
  var DEG = '°', DOT = '·', EN = '–', DEGS = '°/s';

  // ---- LX-16A servo backend DOM refs (advanced panel) ----
  var bkndCur = $('bkndCur'), bkndToggle = $('bkndToggle'), bkndMsg = $('bkndMsg');
  var lxcards = document.querySelectorAll('.lxcard');

  // ---- Config tab groups (user request): one group of cards visible at a time. ----
  // Visibility = (card's data-g == active group) AND (lxcard implies the LX backend is up).
  var cfgGroup = 'rig', lastIsLx = false;
  var cfgTabEls = document.querySelectorAll('#cfgTabs .tab');
  var cfgTabLx  = $('cfgTabLx');
  var svGroup = 'main';                              // Servos sub-tab: main / ids / det
  var svTabEls = document.querySelectorAll('#svTabs .tab');
  function showCfgGroup(){
    // :not(.tab) - the tab BUTTONS also carry data-g (their group identity); without the
    // exclusion this loop hid every tab except the active one (measured: only "Rig" visible).
    Array.prototype.forEach.call(document.querySelectorAll('#adv [data-g]:not(.tab)'), function(el){
      var lxBlocked = el.classList.contains('lxcard') && !lastIsLx;
      var svBlocked = el.hasAttribute('data-sv') && el.getAttribute('data-sv') !== svGroup;
      el.hidden = (el.getAttribute('data-g') !== cfgGroup) || lxBlocked || svBlocked;
    });
    Array.prototype.forEach.call(cfgTabEls, function(b){
      b.classList.toggle('active', b.getAttribute('data-g') === cfgGroup);
    });
    Array.prototype.forEach.call(svTabEls, function(b){
      b.classList.toggle('active', b.getAttribute('data-sv') === svGroup);
    });
  }
  Array.prototype.forEach.call(cfgTabEls, function(b){
    b.addEventListener('click', function(){ cfgGroup = b.getAttribute('data-g'); showCfgGroup(); });
  });
  Array.prototype.forEach.call(svTabEls, function(b){
    b.addEventListener('click', function(){ svGroup = b.getAttribute('data-sv'); showCfgGroup(); });
  });
  showCfgGroup();                                    // initial: Rig group, LX cards hidden
  // ---- control-target tabs: PWM / Serial (LX) / Both — which servos the D-pad drives ----
  var tabEls = document.querySelectorAll('#tabs .tab');
  function setTabs(t, lxUp){
    Array.prototype.forEach.call(tabEls, function(b){
      var v = parseInt(b.getAttribute('data-t'), 10);
      b.classList.toggle('active', v === t);
      b.disabled = (v !== 0 && !lxUp);           // Serial/Both need the LX bus enabled first
    });
  }
  Array.prototype.forEach.call(tabEls, function(b){
    b.addEventListener('click', function(){ send('T,' + b.getAttribute('data-t')); });  // firmware clamps if LX is down
  });
  var telPan = $('telPan'), telTilt = $('telTilt'), telFault = $('telFault');
  var scanHi = $('scanHi'), scanRange = $('scanRange'), scanLone = $('scanLone'),
      scanOut = $('scanOut'), scanSum = $('scanSum');
  var panSel = $('panSel'), tiltSel = $('tiltSel'), idWarn = $('idWarn');
  var joyEn = $('joyEn'), joyMd = $('joyMd'), joySum = $('joySum'), kbWarn = $('kbWarn');
  var lxPin = $('lxPin'), lxPinSave = $('lxPinSave'), lxPinMsg = $('lxPinMsg');
  var lxRelaxSel = $('lxRelaxSel');
  var pwmPanSel = $('pwmPanSel'), pwmTiltSel = $('pwmTiltSel'),
      pwmPinSave = $('pwmPinSave'), pwmPinMsg = $('pwmPinMsg'), pwmSum = $('pwmSum');
  var lastLxPin = -1;

  // Digital-servo pin pickers: candidate pool minus pins used by anything else (the LX
  // bus pin and the OTHER servo's selection). The firmware re-validates on save - this
  // filter is convenience, not the authority.
  var PWM_POOL = [4, 5, 13, 14, 17, 18, 19, 23, 25, 26, 27, 32, 33];
  function rebuildPinSel(sel, other){
    if (!sel || sel === document.activeElement) return;
    var keep = sel.value;
    sel.innerHTML = '';
    PWM_POOL.forEach(function(p){
      var used = (p === lastLxPin) || (other && String(p) === other.value && String(p) !== keep);
      if (used && String(p) !== keep) return;          // skip used pins; never orphan the current value
      var o = document.createElement('option');
      o.value = String(p); o.textContent = 'GPIO ' + p;
      sel.appendChild(o);
    });
    if (keep) sel.value = keep;
  }
  if (pwmPanSel)  pwmPanSel.addEventListener('change',  function(){ rebuildPinSel(pwmTiltSel, pwmPanSel); });
  if (pwmTiltSel) pwmTiltSel.addEventListener('change', function(){ rebuildPinSel(pwmPanSel, pwmTiltSel); });
  if (pwmPinSave) pwmPinSave.addEventListener('click', function(){
    if (!pwmPanSel.value || !pwmTiltSel.value){ pwmPinMsg.textContent = 'pick both pins'; return; }
    pwmPinSave.disabled = true; pwmPinMsg.textContent = 'saving…';
    fetch('/setpwmpins?pan=' + pwmPanSel.value + '&tilt=' + pwmTiltSel.value)
      .then(function(r){ return r.text(); }).then(function(t){
        t = String(t).trim();
        if (t.indexOf('fail') === 0){
          pwmPinMsg.textContent = 'rejected — ' + (t.split('\t')[1] || 'unusable pin');
          pwmPinSave.disabled = false; return;
        }
        pwmPinMsg.textContent = 'saved — rebooting onto the new pins…';
      }).catch(function(){                             // the reboot kills the socket: expected
        pwmPinMsg.textContent = 'saved — rebooting onto the new pins…';
      });
  });
  var kbSel = { HM: $('kbHm'), BK: $('kbBk'), EN: $('kbEn'), TG: $('kbTg') };
  var cfgId = $('cfgId'), cfgRead = $('cfgRead'), cfgMsg = $('cfgMsg'),
      cfgAlimMin = $('cfgAlimMin'), cfgAlimMax = $('cfgAlimMax'),
      cfgVlimMin = $('cfgVlimMin'), cfgVlimMax = $('cfgVlimMax'),
      cfgTlim = $('cfgTlim'), cfgMode = $('cfgMode'), cfgSpeed = $('cfgSpeed'),
      cfgTorque = $('cfgTorque'), cfgTrim = $('cfgTrim'), cfgLed = $('cfgLed'), cfgLederr = $('cfgLederr');
  var progOne = $('progOne'), progNew = $('progNew'), progBroadcast = $('progBroadcast'),
      progCur = $('progCur'), progNew2 = $('progNew2'), progTargeted = $('progTargeted'), progMsg = $('progMsg');
  var curBknd = 0;                                      // last-known backend (0 PWM / 1 LX-16A)

  // Friendly text for the fail<TAB>reason codes returned by the servo endpoints.
  function reason(code){
    var m = {
      bad_id:'ID out of range (0–253)', not_single:'need exactly one servo on the bus',
      verify_failed:'write not verified — retry', cur_absent:'current ID not found on bus',
      dup:'new ID already used by another servo', need_confirm:'confirmation required',
      pwm_mode:'switch to the LX-16A backend first', no_id:'no servo ID given',
      bad_field:'unknown field', range:'value out of range', factory_failed:'factory reset failed'
    };
    return m[code] || (code || 'failed');
  }
  // Telemetry formatters: firmware sends -1 for "no valid sample".
  function volts(mv){ mv = +mv; return (isNaN(mv) || mv < 0) ? EN : (mv / 1000).toFixed(1) + 'V'; }
  function degC(c){ c = +c; return (isNaN(c) || c < 0) ? EN : c + DEG + 'C'; }
  function degA(d){ d = +d; return (isNaN(d) || d < 0) ? EN : d + DEG; }
  function tline(v, t, a){ return volts(v) + ' ' + DOT + ' ' + degC(t) + ' ' + DOT + ' act ' + degA(a); }
  // Decode one axis's nibble of the fault bitmask (bits 0-3 pan, 4-7 tilt).
  function axisFault(f, base){
    if (isNaN(f) || f < 0) return '';
    var b = (f >> base) & 0xF, s = [];
    if (!b) return '';
    if (b & 1) s.push('temp'); if (b & 2) s.push('volt'); if (b & 4) s.push('stall'); if (b & 8) s.push('bus');
    return ' <span class="warn">⚠ ' + s.join('/') + '</span>';
  }
  // <select> helpers for the ID-link dropdowns (populated from scan results + bound IDs).
  function ensureOpt(sel, id){
    if (!sel || id === '' || id == null) return;
    id = String(id);
    for (var i = 0; i < sel.options.length; i++){ if (sel.options[i].value === id) return; }
    var o = document.createElement('option'); o.value = id; o.textContent = 'ID ' + id; sel.appendChild(o);
  }
  function setSelIdle(sel, id){
    if (!sel || sel === document.activeElement) return;    // never fight a mid-select user
    ensureOpt(sel, id); sel.value = String(id);
  }
  function iv(el){ var n = parseInt(el.value, 10); return isNaN(n) ? null : n; }

  function online(){ conn.hidden = true; document.body.classList.remove('offline'); }
  function offline(){ conn.hidden = false; document.body.classList.add('offline'); }

  // Never overwrite an input/slider the user is actively editing, so the 700 ms
  // poll (and /action replies) can't fight a value mid-type or a slider mid-drag.
  function setIdle(el, v){ if (el !== document.activeElement) el.value = v; }

  // Parse the unified CSV returned by BOTH /action and /status. Fields 1-13 are the
  // original contract; the rest are APPEND-ONLY additions (backward-compatible - the
  // p.length<13 guard keeps short bodies safe, and later groups are read only when present):
  //   1-13:  mode,ip,pan,tilt,step,panMin,panMax,tiltMin,tiltMax,homePan,homeTilt,homeSpeed,homing
  //   14-16: bknd,panid,tiltid   17-23: panVin,panTemp,tiltVin,tiltTemp,panActual,tiltActual,fault
  //   24: ctrlTarget  25: joyPresent  26: joyEnabled  27: joyMode
  //   28-31: button remap (home, back, confirm, toggle) as index 0..4 = A/B/C/D/OK
  //   32: lxpin (LX-16A one-wire bus GPIO, applied at boot)
  //   33: lxRelax (idle torque release: 0 hold / 1 release after 3s)
  function applyState(txt){
    var p = String(txt).split(',');
    if (p.length < 13){ online(); return; }             // ignore short/garbled bodies
    badge.textContent = (p[0] || '?') + ' ' + DOT + ' ' + (p[1] || '');
    if (wCur) wCur.textContent = (p[0] === 'STA') ? ('on ' + (p[1] || '')) : 'hotspot mode';
    panEl.textContent = p[2];
    tiltEl.textContent = p[3];
    if (slider !== document.activeElement){ slider.value = p[4]; stepVal.textContent = p[4] + DEG; }
    setIdle(pmin, p[5]); setIdle(pmax, p[6]);
    setIdle(tmin, p[7]); setIdle(tmax, p[8]);
    setIdle(hpan, p[9]); setIdle(htilt, p[10]);
    if (hspd !== document.activeElement){ hspd.value = p[11]; hspdVal.textContent = p[11] + DEGS; }
    limSum.textContent = 'Pan ' + p[5] + EN + p[6] + ' ' + DOT + ' Tilt ' + p[7] + EN + p[8];
    homingEl.hidden = (p[12] !== '1');                  // field 12 = 1 while a home glide runs

    // ---- appended servo-backend fields 14-23 (PWM sends all -1; guarded so short bodies still work) ----
    if (p.length >= 14){
      curBknd = (p[13] === '1') ? 1 : 0;                // field 14 = bknd (0 PWM / 1 LX-16A)
      var isLx = (curBknd === 1);
      lastIsLx = isLx;                                       // LX cards gated via the group logic
      if (cfgTabLx) cfgTabLx.hidden = !isLx;                 // Servos tab exists only in LX mode
      if (!isLx && cfgGroup === 'lx') cfgGroup = 'rig';      // never leave the user on a dead tab
      showCfgGroup();
      if (p.length >= 24) setTabs(parseInt(p[23], 10) || 0, isLx);   // field 24 = ctrlTarget; tabs reflect it
      if (bkndCur)    bkndCur.textContent    = isLx ? 'LX-16A serial bus' : 'PWM (SG90)';
      if (bkndToggle) bkndToggle.textContent = isLx ? 'Switch to PWM (SG90)' : 'Switch to LX-16A serial bus';
      setSelIdle(panSel, p[14]); setSelIdle(tiltSel, p[15]);   // fields 15/16 = panid/tiltid
      ensureOpt(cfgId, p[14]); ensureOpt(cfgId, p[15]);       // Detail dropdown knows the linked IDs
      if (cfgId && !cfgId.value) cfgId.value = String(p[14]);
      if (idWarn) idWarn.textContent = (p[14] === p[15])
        ? ('⚠ pan and tilt share bus ID ' + p[14] + ' — program distinct IDs below') : '';
      if (isLx && p.length >= 23 && telPan){             // fields 17-23 telemetry, only when bknd==1
        var f = parseInt(p[22], 10);                     // field 23 = fault bitmask (-1 n/a, 0 OK)
        telPan.innerHTML  = tline(p[16], p[17], p[20]) + axisFault(f, 0);   // 17 vin,18 temp,21 actual
        telTilt.innerHTML = tline(p[18], p[19], p[21]) + axisFault(f, 4);   // 19 vin,20 temp,22 actual
        telFault.textContent = (isNaN(f) || f < 0) ? ''
          : (f === 0 ? 'status OK' : ('FAULT 0x' + ('0' + f.toString(16).toUpperCase()).slice(-2)));
      }
    }

    // ---- appended cockpit fields 25-31: joystick presence/enable/mode + button remap ----
    if (p.length >= 31){
      if (joySum) joySum.textContent = (p[24] === '1')
        ? (p[25] === '1' ? 'connected' : 'connected — input disabled') : 'not detected';
      setSelIdle(joyEn, p[25]); setSelIdle(joyMd, p[26]);
      setSelIdle(kbSel.HM, p[27]); setSelIdle(kbSel.BK, p[28]);
      setSelIdle(kbSel.EN, p[29]); setSelIdle(kbSel.TG, p[30]);
      var mp = [p[27], p[28], p[29], p[30]];               // warn (don't auto-fix) on a collision
      var dup = mp.some(function(v, i){ return mp.indexOf(v) !== i; });
      if (kbWarn) kbWarn.textContent = dup
        ? '⚠ two actions share one button — it will fire both' : '';
    }
    if (p.length >= 32) setIdle(lxPin, p[31]);          // field 32 = live LX bus GPIO
    if (p.length >= 33) setSelIdle(lxRelaxSel, p[32]);  // field 33 = idle torque release
    if (p.length >= 35){                                // fields 34/35 = PWM pan/tilt GPIOs
      lastLxPin = parseInt(p[31], 10);
      if (pwmPanSel  && !pwmPanSel.value)  { rebuildPinSel(pwmPanSel,  null); pwmPanSel.value  = p[33]; }
      if (pwmTiltSel && !pwmTiltSel.value) { rebuildPinSel(pwmTiltSel, null); pwmTiltSel.value = p[34]; }
      rebuildPinSel(pwmPanSel, pwmTiltSel);
      rebuildPinSel(pwmTiltSel, pwmPanSel);
      if (pwmSum) pwmSum.textContent = 'pan ' + p[33] + ' · tilt ' + p[34];
    }
    online();
  }

  // Every command goes through /action; the reply is the same 31-field CSV (13 base + appended backend/cockpit fields).
  function send(cmd){
    busy = true;
    fetch('/action?go=' + encodeURIComponent(cmd))
      .then(function(r){ return r.text(); })
      .then(applyState)
      .catch(offline)                                  // network error -> disconnected state
      .then(function(){ busy = false; });              // clears after success OR handled error
  }

  function stopRepeat(){ if (timer !== null){ clearInterval(timer); timer = null; } }
  function startRepeat(cmd){
    stopRepeat();                                      // never stack intervals
    send(cmd);                                         // fire the first nudge immediately
    timer = setInterval(function(){ if (!busy){ send(cmd); } }, REPEAT_MS);  // skip ticks while a request is in flight
  }

  // Direction pads: Pointer Events + press-and-hold auto-repeat.
  var dirs = document.querySelectorAll('.dir');
  Array.prototype.forEach.call(dirs, function(btn){
    var cmd = btn.getAttribute('data-cmd');
    btn.addEventListener('pointerdown',   function(e){ e.preventDefault(); startRepeat(cmd); });
    btn.addEventListener('pointerup',     function(e){ e.preventDefault(); stopRepeat(); });
    btn.addEventListener('pointerleave',  stopRepeat);
    btn.addEventListener('pointercancel', stopRepeat);
    btn.addEventListener('contextmenu',   function(e){ e.preventDefault(); });  // no long-press menu mid-hold
    // Keyboard: a <button> is activated by Enter/Space via a synthesized click that fires no
    // pointer events; handle keydown so physical-keyboard users can drive the pad too.
    btn.addEventListener('keydown', function(e){
      if (e.key === 'Enter' || e.key === ' '){ e.preventDefault(); send(cmd); }
    });
  });

  // Backstops so a held pad can never get "stuck" repeating.
  window.addEventListener('blur', stopRepeat);
  document.addEventListener('visibilitychange', function(){ if (document.hidden){ stopRepeat(); } });

  // HOME: single shot -> starts the smooth, non-blocking server-side glide (B,H). Never repeats.
  home.addEventListener('pointerdown', function(e){ e.preventDefault(); send('B,H'); });
  home.addEventListener('contextmenu', function(e){ e.preventDefault(); });
  home.addEventListener('keydown', function(e){        // keyboard activation (see D-pad note above)
    if (e.key === 'Enter' || e.key === ' '){ e.preventDefault(); send('B,H'); }
  });

  // Speed / Step slider: live label on input, send S,<n> on change.
  slider.addEventListener('input',  function(){ stepVal.textContent = slider.value + DEG; });
  slider.addEventListener('change', function(){ send('S,' + slider.value); });

  // Home-speed slider: live label on input, send V,<n> on change.
  hspd.addEventListener('input',  function(){ hspdVal.textContent = hspd.value + DEGS; });
  hspd.addEventListener('change', function(){ send('V,' + hspd.value); });

  // Numeric limit/home fields send N,<what>,<v> on change (firmware clamps + re-clamps).
  Array.prototype.forEach.call(document.querySelectorAll('input[data-set]'), function(inp){
    inp.addEventListener('change', function(){
      var v = parseInt(inp.value, 10);
      if (isNaN(v)) return;                            // don't send junk; server clamps the rest
      send(inp.getAttribute('data-set') + ',' + v);
    });
  });

  // Capture ("Set = current" / "Set Home = current") buttons: single shot on click.
  Array.prototype.forEach.call(document.querySelectorAll('.act[data-cmd]'), function(b){
    b.addEventListener('click', function(){ send(b.getAttribute('data-cmd')); });
  });

  // Reset calibration behind a confirm() so it can't be hit by accident.
  reset.addEventListener('click', function(){
    if (window.confirm('Reset all limits, home and speed to defaults?')) send('C,RS');
  });

  // Badge doubles as the advanced-settings toggle (WiFi, limits, home, reset).
  badge.addEventListener('click', function(){ adv.hidden = !adv.hidden; });

  // ---- WiFi provisioning ----
  wScan.addEventListener('click', function(){
    wMsg.textContent = 'scanning…'; wScan.disabled = true;
    fetch('/scan').then(function(r){ return r.text(); }).then(function(t){
      var lines = t.split('\n').filter(Boolean);
      wSsid.innerHTML = lines.length ? '' : '<option value="">(none found)</option>';
      lines.forEach(function(ln){
        var f = ln.split('\t');
        var o = document.createElement('option');
        o.value = f[0];
        o.textContent = f[0] + '  ' + (f[2] === '1' ? '(locked) ' : '') + f[1] + 'dBm';
        wSsid.appendChild(o);
      });
      wMsg.textContent = lines.length + ' network(s) found';
    }).catch(function(){ wMsg.textContent = 'scan failed'; })
      .then(function(){ wScan.disabled = false; });
  });

  wJoin.addEventListener('click', function(){
    var s = wSsid.value;
    if (!s){ wMsg.textContent = 'pick a network first'; return; }
    fetch('/setwifi?ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(wPass.value))
      .catch(function(){})                             // the reboot drops the socket; expected
      .then(function(){
        wMsg.textContent = 'Saved. Rebooting to join "' + s + '". If it connects, its new IP '
          + 'shows on the OLED (or check your router). If it fails, it comes back as the '
          + 'ESP32-PanTilt hotspot.';
      });
  });

  wForget.addEventListener('click', function(){
    if (!window.confirm('Forget saved WiFi and return to hotspot only?')) return;
    fetch('/forgetwifi').catch(function(){})
      .then(function(){ wMsg.textContent = 'Forgotten. Rebooting as the ESP32-PanTilt hotspot.'; });
  });

  // ---- LX-16A servo backend (advanced) ----
  // Bespoke fetch() handlers modeled on the WiFi block: the servo endpoints reply with
  // TSV / key=value / ok<TAB>… (NOT the status CSV), so their replies never reach applyState.

  // Backend toggle: /setbackend?b=0|1 -> persists bknd + reboots (mirrors /setwifi save-join).
  bkndToggle.addEventListener('click', function(){
    var to = (curBknd === 1) ? 0 : 1;
    var nm = to ? 'LX-16A serial bus' : 'PWM (SG90)';
    if (!window.confirm('Switch servo backend to ' + nm + '?\nThe board will reboot.')) return;
    fetch('/setbackend?b=' + to).catch(function(){})   // the reboot drops the socket; expected
      .then(function(){ bkndMsg.textContent = 'Saved. Rebooting into ' + nm + '… reconnect after a few seconds.'; });
  });

  // Bus scan (/servoscan): range (1..hi) -> "id<TAB>vin<TAB>temp<TAB>pos" lines; lone -> single "<id>".
  function renderScan(text){
    var lines = String(text).split('\n').filter(Boolean);
    if (lines.length && lines[0].indexOf('fail') === 0){
      scanOut.textContent = reason((lines[0].split('\t')[1] || '')); scanSum.textContent = ''; return;
    }
    if (!lines.length){ scanOut.textContent = 'no servos responded'; scanSum.textContent = '0 found'; return; }
    scanOut.innerHTML = '';
    lines.forEach(function(ln){
      var f = ln.split('\t');                            // id, vinMv, tempC, posDeg
      var d = document.createElement('div');
      d.textContent = 'ID ' + f[0] + '  ' + DOT + ' ' + volts(f[1]) + ' ' + DOT + ' ' + degC(f[2]) + ' ' + DOT + ' ' + degA(f[3]);
      scanOut.appendChild(d);
      ensureOpt(panSel, f[0]); ensureOpt(tiltSel, f[0]); ensureOpt(cfgId, f[0]);
    });
    scanSum.textContent = lines.length + ' found';
  }
  scanRange.addEventListener('click', function(){
    var hi = iv(scanHi); if (hi === null) hi = 30;
    scanOut.textContent = 'scanning…'; scanRange.disabled = true; scanLone.disabled = true;
    fetch('/servoscan?mode=range&hi=' + hi).then(function(r){ return r.text(); })
      .then(renderScan).catch(function(){ scanOut.textContent = 'scan failed'; })
      .then(function(){ scanRange.disabled = false; scanLone.disabled = false; });
  });
  scanLone.addEventListener('click', function(){
    scanOut.textContent = 'scanning…'; scanRange.disabled = true; scanLone.disabled = true;
    fetch('/servoscan?mode=lone').then(function(r){ return r.text(); }).then(function(t){
      t = String(t).trim();
      if (t.indexOf('fail') === 0){ scanOut.textContent = reason((t.split('\t')[1] || '')); return; }
      var id = parseInt(t, 10);
      if (!id){ scanOut.textContent = 'none / ambiguous — connect exactly one servo'; scanSum.textContent = ''; return; }
      scanOut.textContent = 'lone servo: ID ' + id; scanSum.textContent = '1 found';
      ensureOpt(panSel, id); ensureOpt(tiltSel, id); ensureOpt(cfgId, id);
    }).catch(function(){ scanOut.textContent = 'scan failed'; })
      .then(function(){ scanRange.disabled = false; scanLone.disabled = false; });
  });

  // Link ID -> axis: EXPLICIT <select> change handlers (data-set matches <input> only, never
  // <select>). Routes through /action so the reply is the status CSV and panid/tiltid echo back.
  if (panSel)  panSel.addEventListener('change',  function(){ if (panSel.value !== '')  send('M,PI,' + panSel.value); });
  if (tiltSel) tiltSel.addEventListener('change', function(){ if (tiltSel.value !== '') send('M,TI,' + tiltSel.value); });

  // Joystick: enable (J), drive mode (Y) and the button remap (K,<action>,<0..4>). Remapping
  // lives ONLY here by design (D-B) - a bad map can never lock you out of the on-device menu.
  var BTN_NAMES = ['A', 'B', 'C', 'D', 'OK (centre-click)'];
  Object.keys(kbSel).forEach(function(act){
    var sel = kbSel[act];
    if (!sel) return;
    BTN_NAMES.forEach(function(n, i){
      var o = document.createElement('option'); o.value = String(i); o.textContent = n; sel.appendChild(o);
    });
    sel.addEventListener('change', function(){ send('K,' + act + ',' + sel.value); });
  });
  if (joyEn) joyEn.addEventListener('change', function(){ send('J,' + joyEn.value); });
  if (joyMd) joyMd.addEventListener('change', function(){ send('Y,' + joyMd.value); });
  if (lxRelaxSel) lxRelaxSel.addEventListener('change', function(){ send('R,' + lxRelaxSel.value); });

  // LX bus pin: persisted to NVS and applied on the next boot, so the board reboots.
  // The firmware validates the pin and answers "fail<TAB>reason" if it isn't usable.
  if (lxPinSave) lxPinSave.addEventListener('click', function(){
    var v = iv(lxPin);
    if (v === null){ lxPinMsg.textContent = 'enter a GPIO number'; return; }
    lxPinSave.disabled = true; lxPinMsg.textContent = 'saving…';
    fetch('/setlxpin?pin=' + v).then(function(r){ return r.text(); }).then(function(t){
      t = String(t).trim();
      if (t.indexOf('fail') === 0){
        lxPinMsg.textContent = 'rejected — ' + (t.split('\t')[1] || 'unusable pin');
        lxPinSave.disabled = false; return;
      }
      lxPinMsg.textContent = 'saved — rebooting onto GPIO ' + v + '…';
    }).catch(function(){                       // the reboot kills the socket: expected
      lxPinMsg.textContent = 'saved — rebooting onto GPIO ' + v + '…';
    });
  });

  // Per-servo config (/servocfg): read = key=value lines; write one field = "ok" / "fail<TAB>reason".
  function cfgWrite(field, v, v2, okMsg){
    var url = '/servocfg?id=' + encodeURIComponent(cfgId.value) + '&set=' + field;
    if (v  !== undefined && v  !== null) url += '&v='  + encodeURIComponent(v);
    if (v2 !== undefined && v2 !== null) url += '&v2=' + encodeURIComponent(v2);
    cfgMsg.textContent = 'writing…';
    fetch(url).then(function(r){ return r.text(); }).then(function(t){
      t = String(t).trim();
      cfgMsg.textContent = (t === 'ok') ? (okMsg || 'ok') : ('fail: ' + reason((t.split('\t')[1] || t)));
    }).catch(function(){ cfgMsg.textContent = 'write failed'; });
  }
  cfgRead.addEventListener('click', function(){
    cfgMsg.textContent = 'reading…';
    fetch('/servocfg?id=' + encodeURIComponent(cfgId.value)).then(function(r){ return r.text(); }).then(function(t){
      if (String(t).indexOf('fail') === 0){ cfgMsg.textContent = reason((String(t).split('\t')[1] || '')); return; }
      var kv = {};
      String(t).split('\n').forEach(function(ln){ var i = ln.indexOf('='); if (i > 0) kv[ln.slice(0, i)] = ln.slice(i + 1); });
      var pair = function(s){ return String(s == null ? '' : s).split(','); };
      if (kv.alim != null){ var a = pair(kv.alim); setIdle(cfgAlimMin, a[0]); setIdle(cfgAlimMax, a[1]); }
      if (kv.vlim != null){ var vv = pair(kv.vlim); setIdle(cfgVlimMin, vv[0]); setIdle(cfgVlimMax, vv[1]); }
      if (kv.tlim != null) setIdle(cfgTlim, kv.tlim);
      if (kv.mode != null){ var m = pair(kv.mode); if (cfgMode !== document.activeElement && (m[0] === '0' || m[0] === '1')) cfgMode.value = m[0]; setIdle(cfgSpeed, m[1]); }
      if (kv.torque != null && cfgTorque !== document.activeElement && (kv.torque === '0' || kv.torque === '1')) cfgTorque.value = kv.torque;
      if (kv.trim != null) setIdle(cfgTrim, kv.trim);
      if (kv.led != null && cfgLed !== document.activeElement && (kv.led === '0' || kv.led === '1')) cfgLed.value = kv.led;
      if (kv.lederr != null) setIdle(cfgLederr, kv.lederr);
      cfgMsg.textContent = 'read — pos ' + (kv.pos != null ? kv.pos + DEG : '?') + ' ' + DOT + ' ' + volts(kv.vin) + ' ' + DOT + ' ' + degC(kv.temp);
    }).catch(function(){ cfgMsg.textContent = 'read failed'; });
  });
  $('cfgSetAlim').addEventListener('click', function(){ var a = iv(cfgAlimMin), b = iv(cfgAlimMax); if (a === null || b === null){ cfgMsg.textContent = 'enter min and max ticks'; return; } cfgWrite('alim', a, b, 'angle limits set'); });
  $('cfgSetVlim').addEventListener('click', function(){ var a = iv(cfgVlimMin), b = iv(cfgVlimMax); if (a === null || b === null){ cfgMsg.textContent = 'enter min and max mV'; return; } cfgWrite('vlim', a, b, 'voltage limits set'); });
  $('cfgSetTlim').addEventListener('click', function(){ var v = iv(cfgTlim); if (v === null){ cfgMsg.textContent = 'enter a temperature'; return; } cfgWrite('tlim', v, null, 'temp cap set'); });
  $('cfgSetMode').addEventListener('click', function(){ var s = iv(cfgSpeed); cfgWrite('mode', cfgMode.value, s === null ? 0 : s, 'mode set'); });
  $('cfgSetTorque').addEventListener('click', function(){
    var v = cfgTorque.value;
    if (v !== '1' && !window.confirm('Unload torque on servo ' + cfgId.value + '?\nA mounted rig may drop.')) return;
    cfgWrite('torque', v, null, v === '1' ? 'torque loaded' : 'torque unloaded');
  });
  $('cfgSetTrim').addEventListener('click', function(){ var v = iv(cfgTrim); if (v === null){ cfgMsg.textContent = 'enter a trim value'; return; } cfgWrite('trim', v, null, 'trim set (RAM)'); });
  $('cfgSaveTrim').addEventListener('click', function(){ cfgWrite('trimsave', null, null, 'trim saved to EEPROM'); });
  $('cfgSetLed').addEventListener('click', function(){ cfgWrite('led', cfgLed.value, null, 'LED set'); });
  $('cfgSetLederr').addEventListener('click', function(){ var v = iv(cfgLederr); if (v === null){ cfgMsg.textContent = 'enter a 0–7 mask'; return; } cfgWrite('lederr', v, null, 'alarm mask set'); });
  $('cfgFactory').addEventListener('click', function(){
    if (!window.confirm('Factory-reset servo ' + cfgId.value + '?\nRestores angle 0–1000, voltage and temperature defaults.')) return;
    cfgWrite('factory', null, null, 'factory defaults restored');
  });

  // Safe ID programming (/servoid). Flow A: broadcast, gated on the "one servo" checkbox +
  // confirm(); firmware authenticates single-servo via mode=lone. Flow B: targeted (cur given),
  // firmware does the duplicate pre-check. Reply is "ok<TAB><id>" / "fail<TAB><reason>".
  function progDone(t){
    var f = String(t).trim().split('\t');
    if (f[0] === 'ok'){ progMsg.textContent = 'done — servo is now ID ' + f[1]; ensureOpt(panSel, f[1]); ensureOpt(tiltSel, f[1]); ensureOpt(cfgId, f[1]); }
    else progMsg.textContent = 'fail: ' + reason((f[1] || ''));
  }
  progBroadcast.addEventListener('click', function(){
    if (!progOne.checked){ progMsg.textContent = 'tick “only one servo connected” first'; return; }
    var n = iv(progNew); if (n === null){ progMsg.textContent = 'enter a new ID (0–253)'; return; }
    if (!window.confirm('Broadcast-program the single connected servo to ID ' + n + '?')) return;
    progMsg.textContent = 'programming…';
    fetch('/servoid?new=' + n + '&confirm=1').then(function(r){ return r.text(); }).then(progDone)
      .catch(function(){ progMsg.textContent = 'request failed'; });
  });
  progTargeted.addEventListener('click', function(){
    var c = iv(progCur), n = iv(progNew2);
    if (c === null || n === null){ progMsg.textContent = 'enter current and new IDs'; return; }
    if (!window.confirm('Re-ID servo ' + c + ' → ' + n + '?')) return;
    progMsg.textContent = 'programming…';
    fetch('/servoid?new=' + n + '&confirm=1&cur=' + c).then(function(r){ return r.text(); }).then(progDone)
      .catch(function(){ progMsg.textContent = 'request failed'; });
  });

  // Poll /status so the page reflects the autonomous home glide and any change
  // made elsewhere. applyState skips focused fields, so polling never clobbers a
  // value the user is mid-edit. Skip while an /action is already in flight.
  function poll(){
    if (busy) return;
    fetch('/status')
      .then(function(r){ return r.text(); })
      .then(applyState)
      .catch(offline);
  }
  poll();                                              // initial fill on load
  setInterval(poll, POLL_MS);
})();
</script>
</body>
</html>
)rawliteral";
