use wef::{should_shutdown, Value, Window};
use std::time::Duration;

fn hello_main() {
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(async {
        let _win = Window::new(800, 600)
            .title("WEF - Bindings Demo")
            .bind("greet", |call| {
                let name = call.args.get(0)
                    .and_then(|v| v.as_string())
                    .unwrap_or("World")
                    .to_string();
                call.resolve(Value::String(format!("Hello, {}!", name)));
            })
            .bind("add", |call| {
                let a = call.args.get(0).and_then(|v| v.as_int()).unwrap_or(0);
                let b = call.args.get(1).and_then(|v| v.as_int()).unwrap_or(0);
                call.resolve(Value::Int(a + b));
            })
            .bind("getInfo", |call| {
                let mut info = std::collections::HashMap::new();
                info.insert("name".to_string(), Value::String("WEF".to_string()));
                info.insert("version".to_string(), Value::String("0.1.0".to_string()));
                info.insert("rust".to_string(), Value::Bool(true));
                call.resolve(Value::Dict(info));
            })
            .load_url(r#"data:text/html,<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <style>
        body {
            font-family: system-ui, -apple-system, sans-serif;
            max-width: 600px;
            margin: 40px auto;
            padding: 20px;
            background: linear-gradient(135deg, %231a1a2e, %2316213e);
            color: white;
            min-height: 100vh;
        }
        h1 { color: %2300d4ff; }
        button {
            background: %2300d4ff;
            color: black;
            border: none;
            padding: 10px 20px;
            margin: 5px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 14px;
        }
        button:hover { background: %2300a8cc; }
        pre {
            background: rgba(0,0,0,0.3);
            padding: 15px;
            border-radius: 5px;
            overflow-x: auto;
        }
        .result { color: %2300ff88; }
        .error { color: %23ff6b6b; }
    </style>
</head>
<body>
    <h1>WEF Bindings</h1>
    <p>Call Rust functions from JavaScript:</p>
    <div>
        <button onclick="testGreet()">Deno.greet("Alice")</button>
        <button onclick="testAdd()">Deno.add(10, 25)</button>
        <button onclick="testInfo()">Deno.getInfo()</button>
        <button onclick="testUnknown()">Deno.unknown()</button>
    </div>
    <pre id="output">Click a button to test...</pre>
    <script>
        const out = document.getElementById('output');
        function log(msg, isError) {
            out.innerHTML += '<div class="' + (isError ? 'error' : 'result') + '">' + msg + '</div>';
        }
        async function testGreet() {
            try {
                const result = await Deno.greet('Alice');
                log('greet: ' + result);
            } catch(e) { log('Error: ' + e.message, true); }
        }
        async function testAdd() {
            try {
                const result = await Deno.add(10, 25);
                log('add: ' + result);
            } catch(e) { log('Error: ' + e.message, true); }
        }
        async function testInfo() {
            try {
                const result = await Deno.getInfo();
                log('getInfo: ' + JSON.stringify(result));
            } catch(e) { log('Error: ' + e.message, true); }
        }
        async function testUnknown() {
            try {
                const result = await Deno.unknown();
                log('unknown: ' + result);
            } catch(e) { log('Error: ' + e.message, true); }
        }
        out.innerHTML = 'Ready! Click buttons above.\n';
    </script>
</body>
</html>"#);

        while !should_shutdown() {
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    });
}

wef::main!(hello_main);
