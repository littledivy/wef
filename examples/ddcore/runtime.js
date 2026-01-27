const {BrowserWindow} = Deno.core.ops;

const win = new BrowserWindow(1024, 768);
win.setTitle("DDCore App");
win.loadUrl(`data:text/html,
<!DOCTYPE html>
<html>
<head>
  <style>
    body {
      font-family: system-ui;
      display: flex;
      align-items: center;
      justify-content: center;
      height: 100vh;
      margin: 0;
      background: linear-gradient(135deg, %231a1a2e, %2316213e);
      color: white;
    }
    h1 { color: %2300d4ff; }
  </style>
</head>
<body>
  <div>
    <h1>DDCore</h1>
    <p>Window created from deno_core JavaScript!</p>
  </div>
</body>
</html>`);

console.log("[DDCore] BrowserWindow created");
