// birthday.js — a little cake with flickering candles, made for her birthday.
//
// Tilt the device to "blow" on the flames — they lean with the tilt.
// The middle layer + drips pick up the accent color from the picker, so
// you can match it to her favorite color from the companion app.

let blow = 0;

function setup() {
  autoRotate();
}

function draw() {
  background(15, 5, 22);

  // Smooth the tilt so blowing is gentle, not snappy.
  // EMA: weights must sum to 1 or `blow` accumulates ~8× the input
  // and the flames slide off their candles.
  blow = blow * 0.88 + accelX * 5 * 0.12;

  // ─── sparkles drifting around the rim ───
  noStroke();
  fill(255, 240, 180);
  for (let i = 0; i < 10; i++) {
    let a = frameCount * 0.008 + i * (TWO_PI / 10);
    let r = 100 + sin(frameCount * 0.04 + i * 2) * 4;
    circle(120 + cos(a) * r, 120 + sin(a) * r, 2);
  }

  // ─── cake (three layers, theme-colored middle) ───
  // Bottom — cream
  fill(255, 220, 200);
  rect(50, 168, 140, 30);
  ellipse(120, 168, 140, 18);

  // Middle — accent color
  fill(themeR, themeG, themeB);
  rect(65, 138, 110, 30);
  ellipse(120, 138, 110, 18);

  // Top — cream
  fill(255, 220, 200);
  rect(82, 112, 76, 26);
  ellipse(120, 112, 76, 14);

  // Frosting drips down the bottom layer
  fill(themeR, themeG, themeB);
  for (let i = 0; i < 6; i++) {
    let x = 60 + i * 22;
    let h = 8 + (i % 3) * 4;
    rect(x - 4, 168, 8, h);
    ellipse(x, 168 + h, 8, 6);
  }

  // ─── candles ───
  // Three white candles on the top layer.
  for (let i = 0; i < 3; i++) {
    let cx = 100 + i * 20;
    let baseY = 100;

    // Wax
    fill(255, 240, 240);
    rect(cx - 2, baseY, 4, 12);

    // Wick stub
    fill(50, 30, 20);
    rect(cx - 0.5, baseY - 3, 1, 3);

    // Flame — flickers via sin + leans with the smoothed tilt.
    let flicker = sin(frameCount * 0.25 + i * 1.7) * 1.2;
    let tx = blow * 0.35;

    // Outer flame (warm orange)
    fill(255, 140, 40);
    ellipse(cx + tx, baseY - 7 + flicker, 5, 10);
    // Inner core (pale gold)
    fill(255, 230, 150);
    ellipse(cx + tx * 0.7, baseY - 6 + flicker, 2, 6);
  }

  // ─── greeting ───
  fill(255, 240, 200);
  textSize(16);
  textAlign(CENTER);
  text("Happy", 120, 45);
  fill(themeR, themeG, themeB);
  text("Birthday!", 120, 63);
  textSize(20);
  text("Q", 120, 100);
}
