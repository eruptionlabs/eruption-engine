// Liga o tipo de depuração "eruption" à porta do editor do Eruption Engine,
// que fala o Debug Adapter Protocol direto (nada roda aqui além disso).
const vscode = require('vscode');

function activate(context) {
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('eruption', {
        createDebugAdapterDescriptor(session) {
            const port = session.configuration.port || 4711;
            return new vscode.DebugAdapterServer(port, '127.0.0.1');
        }
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
