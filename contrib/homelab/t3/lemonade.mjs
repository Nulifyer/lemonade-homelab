// Translate Pi's thinking selector using the checkpoint's documented template
// controls. Sampling stays with the native model profile.
export default function (pi) {
  pi.on('before_provider_request', (event, ctx) => {
    if (ctx.model?.provider !== 'homelab') return;
    const payload = { ...event.payload };
    const enabled = ctx.model.reasoning !== false && pi.getThinkingLevel() !== 'off';
    payload.chat_template_kwargs = {
      ...payload.chat_template_kwargs, enable_thinking: enabled,
    };
    const nex = ctx.model.id.replace(/^user\./, '') === 'Select-Nex25-Q4'
      || (process.env.HOMELAB_REASONING_TEMPLATE === 'effort'
        && ['agent-work', 'chat-roleplay'].includes(ctx.model.id));
    if (nex) {
      payload.chat_template_kwargs.reasoning_effort = enabled ? 'high' : 'none';
      if (enabled) {
        payload.reasoning_effort = 'high';
        payload.reasoning_budget_tokens = 1024;
      } else {
        // b10944 erases the Jinja effort when top-level effort is 'none'.
        delete payload.reasoning_effort;
      }
    }
    return payload;
  });
}
