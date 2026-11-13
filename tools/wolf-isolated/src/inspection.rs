//! Retained data-only expectation from root-prepared config and native metadata, never inspect output.
#[derive(Clone)]
pub struct Policy {
    pub(crate) uid: u32,
    pub(crate) image: String,
    pub(crate) runtime: String,
    pub(crate) state: String,
    pub(crate) prefix: String,
    pub(crate) game: String,
    pub(crate) helper: String,
    pub(crate) gpu: Vec<String>,
    pub(crate) rules: Vec<String>,
}
impl Policy {
    pub(crate) fn new(
        p: &crate::Profile,
        state: &str,
        helper: &str,
        gpu: Vec<String>,
        rules: Vec<String>,
        image: &str,
    ) -> Self {
        Self {
            uid: p.remote_uid,
            image: image.into(),
            runtime: p.runtime.clone(),
            state: state.into(),
            prefix: p.prefix.working.clone(),
            game: p.game_data.clone(),
            helper: helper.into(),
            gpu,
            rules,
        }
    }
}
