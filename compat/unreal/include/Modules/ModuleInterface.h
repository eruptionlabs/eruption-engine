#pragma once

// Interface de modulo (camada compat): o host chama StartupModule depois de
// carregar a biblioteca e ShutdownModule antes de descarrega-la.

class IModuleInterface {
public:
    virtual ~IModuleInterface() = default;
    virtual void StartupModule() {}
    virtual void PreUnloadCallback() {}
    virtual void PostLoadCallback() {}
    virtual void ShutdownModule() {}
    virtual bool SupportsDynamicReloading() { return true; }
    virtual bool SupportsAutomaticShutdown() { return true; }
    virtual bool IsGameModule() const { return false; }
};

class FDefaultModuleImpl : public IModuleInterface {};
class FDefaultGameModuleImpl : public FDefaultModuleImpl {
public:
    bool IsGameModule() const override { return true; }
};
