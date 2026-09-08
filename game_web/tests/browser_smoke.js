async (page) => {
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.unrouteAll({ behavior: 'ignoreErrors' });
  await page.route('**/*', route => route.request().url().startsWith('http://127.0.0.1:8000/')
    ? route.continue() : route.abort());
  await page.goto('http://127.0.0.1:8000');
  await page.waitForFunction(() => window.gameWebDiagnostics?.().meshCount === 1);
  // The standard-library bridge only serves public assets, so supply the fixture
  // via the tool's file upload API or run this function with the repository cwd.
  for (let i = 0; i < 12; i++) {
    await page.locator('#file').setInputFiles('game_web/tests/blender_snapshot.bin');
    await page.waitForFunction(() => window.gameWebDiagnostics().source === 'file');
    await page.locator('#live').click();
    await page.waitForFunction(() => window.gameWebDiagnostics().source === 'live'
      && window.gameWebDiagnostics().revision >= 0);
  }
  const before = await page.evaluate(() => window.gameWebDiagnostics());
  await page.evaluate(() => {
    const input = document.getElementById('file');
    const transfer = new DataTransfer();
    transfer.items.add(new File(['invalid'], 'invalid.bin'));
    input.files = transfer.files;
    input.dispatchEvent(new Event('change'));
  });
  await page.waitForFunction(() => !document.getElementById('error').hidden);
  const after = await page.evaluate(() => window.gameWebDiagnostics());
  if (JSON.stringify(before) !== JSON.stringify(after)) throw new Error('Invalid file changed scene');
  if (after.geometries !== 1) throw new Error('GPU geometry leak');
  if (JSON.stringify(after.cameraUp) !== '[0,0,1]') throw new Error('Expected Z-up');
  await page.setViewportSize({ width: 600, height: 700 });
  await page.locator('#frame').click();
  await page.waitForFunction(() => window.gameWebDiagnostics().aspect < 1.1);
  if (errors.length) throw new Error(errors.join('\n'));
  await page.locator('#live').click();
  return { before, after, resized: await page.evaluate(() => window.gameWebDiagnostics()), errors };
}
