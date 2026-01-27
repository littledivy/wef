use deno_core::op2;
use deno_core::v8;
use deno_core::GarbageCollected;
use wef::{navigate, quit, set_title, set_window_size, should_shutdown};

struct BrowserWindow;

unsafe impl GarbageCollected for BrowserWindow {
   fn trace(&self, _visitor: &mut v8::cppgc::Visitor) {}

  fn get_name(&self) -> &'static std::ffi::CStr {
    c"BrowserWindow"
  }
}

#[op2]
impl BrowserWindow {
    #[constructor]
    #[cppgc]
    fn new(#[smi] width: i32, #[smi] height: i32) -> BrowserWindow {
        set_window_size(width, height);
        BrowserWindow {}
    }

    #[fast]
    fn load_url(&self,#[string] url: &str) {
        navigate(url);
    }

    #[fast]
    fn set_title(&self,#[string] title: &str) {
        set_title(title);
    }

    #[fast]
    fn set_size(&self,#[smi] width: i32, #[smi] height: i32) {
        set_window_size(width, height);
    }

    #[fast]
    fn quit(&self) {
        quit();
    }
}

deno_core::extension!(
    ddcore,
    objects = [BrowserWindow],
    esm_entry_point = "ext:ddcore/runtime.js",
    esm = ["runtime.js"],
);

wef::main!(|| {
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(async {
        let mut runtime = deno_core::JsRuntime::new(deno_core::RuntimeOptions {
            extensions: vec![ddcore::init()],
            ..Default::default()
        });

        while !should_shutdown() {
            runtime
                .run_event_loop(deno_core::PollEventLoopOptions::default())
                .await
                .ok();
            tokio::time::sleep(std::time::Duration::from_millis(16)).await;
        }
    });
});
