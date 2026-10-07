import React, { useEffect, useRef, useState } from 'react';
import { serverConfig } from './utils/serverConfig';

export interface ResourceBudget {
  available: boolean;
  host_total_gib: number | null;
  host_available_gib: number | null;
  container_current_gib: number | null;
  container_peak_gib: number | null;
  container_limit_gib: number | null;
  container_cpu_limit_cores: number | null;
  accounting: string;
  models: Array<{model_name: string; recipe: string; device: string; pid: number;
    rss_gib: number | null; pss_gib: number | null; gpu_resident_gib: number | null;
    cpu_seconds: number | null; npu_client: boolean}>;
}

const memory = (value: number | null | undefined) => value == null ? 'Unavailable' : `${value.toFixed(2)} GiB`;

const ResourceBudgetModal: React.FC<{budget?: ResourceBudget; onClose: () => void}> = ({budget, onClose}) => {
  const close = useRef<HTMLButtonElement>(null);
  const [updates, setUpdates] = useState<string[]>([]);
  const [checking, setChecking] = useState(false);
  const [message, setMessage] = useState('');
  const refreshUpdates = async () => {
    const response = await serverConfig.fetch('/models');
    if (!response.ok) throw new Error('Cannot read model revisions');
    const data = await response.json();
    setUpdates(data.data.filter((model: any) => model.downloaded && model.update_available).map((model: any) => model.id));
  };
  useEffect(() => {
    const previous = document.activeElement as HTMLElement;
    close.current?.focus();
    refreshUpdates().catch(() => setMessage('Model revision status is unavailable.'));
    const key = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose();
      if (event.key === 'Tab') {
        const buttons = close.current?.closest('[role="dialog"]')?.querySelectorAll<HTMLButtonElement>('button:not(:disabled)');
        if (!buttons?.length) return;
        const first = buttons[0], last = buttons[buttons.length - 1];
        if (event.shiftKey && document.activeElement === first) {event.preventDefault(); last.focus();}
        else if (!event.shiftKey && document.activeElement === last) {event.preventDefault(); first.focus();}
      }
    };
    document.addEventListener('keydown', key);
    return () => {document.removeEventListener('keydown', key); previous?.focus();};
  }, [onClose]);
  const checkUpdates = async () => {
    setChecking(true); setMessage('Checking downloaded models. No weights will be downloaded.');
    try {
      const response = await serverConfig.fetch('/models/check-updates', {method: 'POST'});
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || 'Revision check failed');
      await refreshUpdates();
      const failed = Object.keys(data.failed_models || {}).length;
      setMessage(failed ? `${failed} model checks failed. Retry when their registry is available.` : 'Revision check complete. Updates require your download action.');
    } catch (error) {setMessage(error instanceof Error ? error.message : 'Revision check failed');}
    finally {setChecking(false);}
  };
  return <div className="settings-overlay" onClick={onClose}>
    <section className="settings-modal resource-budget-modal" role="dialog" aria-modal="true" aria-labelledby="resource-budget-title" onClick={e => e.stopPropagation()}>
      <header className="settings-header"><h2 id="resource-budget-title">Resources and model updates</h2><button ref={close} className="settings-close-button" aria-label="Close resources" onClick={onClose}>×</button></header>
      <div className="resource-budget-content">
        {!budget?.available ? <p>Detailed accounting is unavailable on this platform.</p> : <>
          <dl className="resource-budget-summary">
            <dt>Host available RAM</dt><dd>{memory(budget.host_available_gib)} of {memory(budget.host_total_gib)}</dd>
            <dt>Container memory</dt><dd>{memory(budget.container_current_gib)} · limit {memory(budget.container_limit_gib)}</dd>
            <dt>Container peak since start</dt><dd>{memory(budget.container_peak_gib)}</dd>
            <dt>Container CPU limit</dt><dd>{budget.container_cpu_limit_cores ?? 'Unlimited or unavailable'} cores</dd>
          </dl>
          <p>{budget.accounting}</p>
          <div className="resource-budget-table-wrap"><table className="resource-budget-table"><thead><tr><th>Model / runtime</th><th>CPU time</th><th>Process PSS</th><th>GPU resident</th><th>Device</th></tr></thead>
            <tbody>{budget.models.map(model => <tr key={`${model.model_name}-${model.pid}`}><th scope="row">{model.model_name}<small>{model.recipe} · PID {model.pid}</small></th><td>{model.cpu_seconds == null ? 'Unavailable' : `${model.cpu_seconds.toFixed(1)} s`}</td><td>{memory(model.pss_gib)}</td><td>{memory(model.gpu_resident_gib)}</td><td>{model.device}{model.npu_client ? ' · NPU client' : ''}</td></tr>)}</tbody></table></div>
          <p>CPU time is cumulative. PSS shares mapped pages between processes. GPU resident memory may overlap PSS and host RAM.</p>
        </>}
        <h3>Available model revisions</h3>
        {updates.length ? <ul>{updates.map(model => <li key={model}>{model}</li>)}</ul> : <p>No updates reported by the last registry check.</p>}
        <button className="settings-save-button" disabled={checking} onClick={checkUpdates}>{checking ? 'Checking…' : 'Check model updates'}</button>
        <p role="status">{message}</p>
      </div>
    </section>
  </div>;
};
export default ResourceBudgetModal;
