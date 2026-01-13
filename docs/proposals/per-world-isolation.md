# Per-World Isolation for Gameplay Work Balancer

## Problem Statement

The current Gameplay Work Balancer (GWB) implementation uses a **global singleton pattern** where a single `UGWBManager` instance is shared across all worlds. This causes issues in Play-In-Editor (PIE) scenarios with multiple worlds:

1. **Work isolation failure**: Work scheduled from one PIE instance affects other PIE instances
2. **State bleeding**: Global counters (`TotalWorkCount`, `bIsDoingWork`) are shared
3. **Reset conflicts**: When one world tears down, it resets all work from all worlds
4. **Scheduler conflicts**: The scheduler is tied to a single world, causing timer issues when multiple game worlds exist

## Current Architecture

### Singleton Pattern Flow
```
GEngine
  └─ UGWBSubsystem (UEngineSubsystem - one per engine)
       └─ UGWBManager* Manager (single instance)
            ├─ UGWBScheduler* Scheduler (tied to one world)
            ├─ TSet<FGWBWorkGroup> WorkGroups
            ├─ uint32 TotalWorkCount
            └─ bool bIsDoingWork
```

### Key Issues in Current Implementation

1. **`UGWBSubsystem::OnPostWorldInitialization`** (GWBSubsystem.cpp:27-33):
   - Calls `Manager->Initialize(World)` for each game world
   - Overwrites the scheduler on each call, losing reference to previous world's scheduler

2. **`UGWBSubsystem::OnWorldBeginTearDown`** (GWBSubsystem.cpp:35-41):
   - Calls `Manager->Reset()` which clears ALL work from ALL worlds
   - This aborts work units that belong to other still-running worlds

3. **Static API ignores WorldContextObject** (GWBManager.cpp:75-80):
   ```cpp
   FGWBWorkUnitHandle UGWBManager::ScheduleWork(const UObject* WorldContextObject, ...)
   {
       const UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();
       UGWBManager* GlobalManager = Subsystem->GetManager();  // Always returns same manager!
       return GlobalManager->ScheduleWork(WorkGroupId, WorkOptions);
   }
   ```

## Proposed Solution: Per-World Manager Instances

### Architecture Overview

Transform `UGWBSubsystem` to manage a **map of managers keyed by world**:

```
GEngine
  └─ UGWBSubsystem (UEngineSubsystem - one per engine)
       └─ TMap<TWeakObjectPtr<UWorld>, UGWBManager*> WorldManagers
            ├─ World_1 → UGWBManager* (PIE Instance 1)
            │    ├─ UGWBScheduler* (tied to World_1)
            │    ├─ TSet<FGWBWorkGroup>
            │    └─ uint32 TotalWorkCount
            │
            └─ World_2 → UGWBManager* (PIE Instance 2)
                 ├─ UGWBScheduler* (tied to World_2)
                 ├─ TSet<FGWBWorkGroup>
                 └─ uint32 TotalWorkCount
```

### Implementation Changes

#### 1. GWBSubsystem.h - Per-World Manager Storage

```cpp
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GWBSubsystem.generated.h"

class UGWBManager;

UCLASS()
class GWBRUNTIME_API UGWBSubsystem : public UEngineSubsystem
{
    GENERATED_BODY()
public:
    // Begin USubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    // End USubsystem

    /**
     * Get the manager instance for a specific world.
     * @param World The world to get the manager for. If nullptr, returns nullptr.
     * @return The manager for the specified world, or nullptr if not found.
     */
    UGWBManager* GetManagerForWorld(const UWorld* World) const;

    /**
     * Get the manager for a world context object.
     * Extracts the world from the context object and returns the appropriate manager.
     * @param WorldContextObject Object used to determine the world context
     * @return The manager for the world, or nullptr if world cannot be determined
     */
    UGWBManager* GetManagerForContext(const UObject* WorldContextObject) const;

    /**
     * @deprecated Use GetManagerForWorld or GetManagerForContext instead.
     * Returns the first available manager for backwards compatibility.
     * In multi-world scenarios, prefer the world-specific getters.
     */
    UFUNCTION(BlueprintCallable, Category = "GameWorkBalancer",
              meta = (DeprecatedFunction,
                      DeprecationMessage = "Use GetManagerForWorld instead"))
    UGWBManager* GetManager() const;

    void OnPostWorldInitialization(UWorld* World, const UWorld::InitializationValues IVS);
    void OnWorldBeginTearDown(UWorld* World);

private:
    /** Per-world manager instances. Each game world gets its own isolated manager. */
    UPROPERTY()
    TMap<TWeakObjectPtr<UWorld>, TObjectPtr<UGWBManager>> WorldManagers;

    /** Create a new manager for the specified world */
    UGWBManager* CreateManagerForWorld(UWorld* World);

    /** Clean up stale world references (worlds that have been garbage collected) */
    void CleanupStaleWorlds();
};
```

#### 2. GWBSubsystem.cpp - Per-World Manager Implementation

```cpp
#include "GWBSubsystem.h"
#include "GWBManager.h"

void UGWBSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    FWorldDelegates::OnPostWorldInitialization.AddUObject(
        this, &UGWBSubsystem::OnPostWorldInitialization);
    FWorldDelegates::OnWorldBeginTearDown.AddUObject(
        this, &UGWBSubsystem::OnWorldBeginTearDown);
}

void UGWBSubsystem::Deinitialize()
{
    Super::Deinitialize();

    // Reset all managers
    for (auto& Pair : WorldManagers)
    {
        if (Pair.Value)
        {
            Pair.Value->Reset();
        }
    }
    WorldManagers.Empty();
}

UGWBManager* UGWBSubsystem::GetManagerForWorld(const UWorld* World) const
{
    if (!World)
    {
        return nullptr;
    }

    const TWeakObjectPtr<UWorld> WeakWorld =
        const_cast<UWorld*>(World);  // TWeakObjectPtr requires non-const

    if (const TObjectPtr<UGWBManager>* FoundManager = WorldManagers.Find(WeakWorld))
    {
        return *FoundManager;
    }

    return nullptr;
}

UGWBManager* UGWBSubsystem::GetManagerForContext(const UObject* WorldContextObject) const
{
    if (!WorldContextObject)
    {
        return nullptr;
    }

    const UWorld* World = GEngine->GetWorldFromContextObject(
        WorldContextObject, EGetWorldErrorMode::ReturnNull);

    return GetManagerForWorld(World);
}

UGWBManager* UGWBSubsystem::GetManager() const
{
    // Backwards compatibility: return first available manager
    // Log a warning to encourage migration to world-specific API
    if (WorldManagers.Num() > 1)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UGWBSubsystem::GetManager() called with multiple worlds active. "
                 "Consider using GetManagerForWorld() for proper isolation."));
    }

    for (const auto& Pair : WorldManagers)
    {
        if (Pair.Key.IsValid() && Pair.Value)
        {
            return Pair.Value;
        }
    }

    return nullptr;
}

void UGWBSubsystem::OnPostWorldInitialization(UWorld* World,
                                              const UWorld::InitializationValues IVS)
{
    if (!World || !World->IsGameWorld())
    {
        return;
    }

    CleanupStaleWorlds();
    CreateManagerForWorld(World);
}

void UGWBSubsystem::OnWorldBeginTearDown(UWorld* World)
{
    if (!World || !World->IsGameWorld())
    {
        return;
    }

    TWeakObjectPtr<UWorld> WeakWorld = World;

    if (TObjectPtr<UGWBManager>* FoundManager = WorldManagers.Find(WeakWorld))
    {
        if (*FoundManager)
        {
            (*FoundManager)->Reset();
        }
        WorldManagers.Remove(WeakWorld);

        UE_LOG(LogTemp, Verbose,
            TEXT("UGWBSubsystem: Removed manager for world '%s'. Active worlds: %d"),
            *World->GetName(), WorldManagers.Num());
    }
}

UGWBManager* UGWBSubsystem::CreateManagerForWorld(UWorld* World)
{
    check(World);

    TWeakObjectPtr<UWorld> WeakWorld = World;

    // Check if manager already exists
    if (TObjectPtr<UGWBManager>* ExistingManager = WorldManagers.Find(WeakWorld))
    {
        if (*ExistingManager)
        {
            return *ExistingManager;
        }
    }

    // Create new manager owned by the world
    UGWBManager* NewManager = NewObject<UGWBManager>(World);
    NewManager->Initialize(World);

    WorldManagers.Add(WeakWorld, NewManager);

    UE_LOG(LogTemp, Verbose,
        TEXT("UGWBSubsystem: Created manager for world '%s'. Active worlds: %d"),
        *World->GetName(), WorldManagers.Num());

    return NewManager;
}

void UGWBSubsystem::CleanupStaleWorlds()
{
    // Remove entries for garbage-collected worlds
    for (auto It = WorldManagers.CreateIterator(); It; ++It)
    {
        if (!It->Key.IsValid())
        {
            It.RemoveCurrent();
        }
    }
}
```

#### 3. GWBManager.cpp - Update Static API to Use World Context

```cpp
FGWBWorkUnitHandle UGWBManager::ScheduleWork(const UObject* WorldContextObject,
                                             const FName WorkGroupId,
                                             const FGWBWorkOptions& WorkOptions)
{
    UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("GWB Subsystem not available"));
        return FGWBWorkUnitHandle::PassthroughHandle();
    }

    UGWBManager* Manager = Subsystem->GetManagerForContext(WorldContextObject);
    if (!Manager)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("No GWB Manager found for world context. Work will be passed through."));
        return FGWBWorkUnitHandle::PassthroughHandle();
    }

    return Manager->ScheduleWork(WorkGroupId, WorkOptions);
}

void UGWBManager::AbortWorkUnit(const UObject* WorldContextObject,
                                FGWBWorkUnitHandle WorkUnitHandle)
{
    UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();
    if (!Subsystem)
    {
        return;
    }

    UGWBManager* Manager = Subsystem->GetManagerForContext(WorldContextObject);
    if (!Manager)
    {
        return;
    }

    // Search only in this world's work groups
    for (auto ItCategory = Manager->WorkGroups.CreateIterator(); ItCategory; ++ItCategory)
    {
        for (auto& WorkUnit : ItCategory->WorkUnitsQueue)
        {
            if (WorkUnitHandle.GetId() == WorkUnit.GetId())
            {
                WorkUnit.GetAbortCallback().ExecuteIfBound();
                WorkUnit.MarkAborted();
                return;
            }
        }
    }
}
```

#### 4. UGWBManager - Update Outer Object

The manager should be owned by the world to ensure proper garbage collection:

```cpp
void UGWBSubsystem::CreateManagerForWorld(UWorld* World)
{
    // Create manager with World as outer - this ensures:
    // 1. Manager is garbage collected when world is destroyed
    // 2. Manager can access its world via GetOuter<UWorld>()
    UGWBManager* NewManager = NewObject<UGWBManager>(World);
    NewManager->Initialize(World);
    // ...
}
```

Additionally, update `UGWBManager` to optionally get its world from its outer:

```cpp
// Add to GWBManager.h
public:
    /** Get the world this manager is associated with */
    UWorld* GetWorld() const;

// In GWBManager.cpp
UWorld* UGWBManager::GetWorld() const
{
    // Try to get world from outer first
    if (UWorld* OuterWorld = GetTypedOuter<UWorld>())
    {
        return OuterWorld;
    }

    // Fallback to scheduler's world
    if (Scheduler.IsValid())
    {
        return Scheduler->GetWorld();
    }

    return nullptr;
}
```

### Migration Guide

#### For Existing Code

**Before (using global singleton):**
```cpp
// Old way - no world isolation
UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();
UGWBManager* Manager = Subsystem->GetManager();
```

**After (world-aware):**
```cpp
// New way - properly isolated per world
UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();
UGWBManager* Manager = Subsystem->GetManagerForWorld(GetWorld());

// Or use context object
UGWBManager* Manager = Subsystem->GetManagerForContext(this);
```

#### Blueprint Compatibility

The static `ScheduleWork` and `AbortWorkUnit` functions already have `WorldContext="WorldContextObject"` metadata, so Blueprints will continue to work correctly - the world context is automatically passed by the Blueprint VM.

### Testing Considerations

#### New Test Cases

1. **Multi-world isolation test**:
   - Create two worlds
   - Schedule work in each
   - Verify work count is independent per world
   - Tear down one world
   - Verify other world's work is unaffected

2. **World teardown test**:
   - Schedule work in a world
   - Tear down the world
   - Verify manager is cleaned up
   - Verify no dangling references

3. **Scheduler isolation test**:
   - Create two worlds
   - Start schedulers in both
   - Verify each scheduler ticks independently
   - Stop one scheduler
   - Verify other continues

#### Example Test

```cpp
DEFINE_SPEC(FGWBMultiWorldTests, "GWB.MultiWorld",
            EAutomationTestFlags::ProductFilter | EAutomationTestFlags::ApplicationContextMask)

void FGWBMultiWorldTests::Define()
{
    Describe("Per-World Isolation", [this]()
    {
        It("should maintain separate work counts per world", [this]()
        {
            // Create two test worlds
            UWorld* World1 = CreateTestWorld("TestWorld1");
            UWorld* World2 = CreateTestWorld("TestWorld2");

            UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();

            UGWBManager* Manager1 = Subsystem->GetManagerForWorld(World1);
            UGWBManager* Manager2 = Subsystem->GetManagerForWorld(World2);

            // Verify different managers
            TestNotEqual("Managers should be different", Manager1, Manager2);

            // Schedule work in World1
            Manager1->ScheduleWork("Default", FGWBWorkOptions());
            Manager1->ScheduleWork("Default", FGWBWorkOptions());

            // Schedule work in World2
            Manager2->ScheduleWork("Default", FGWBWorkOptions());

            // Verify separate counts
            TestEqual("World1 work count", Manager1->TotalWorkCount, 2u);
            TestEqual("World2 work count", Manager2->TotalWorkCount, 1u);

            // Cleanup
            DestroyTestWorld(World1);
            DestroyTestWorld(World2);
        });

        It("should not affect other worlds on teardown", [this]()
        {
            UWorld* World1 = CreateTestWorld("TestWorld1");
            UWorld* World2 = CreateTestWorld("TestWorld2");

            UGWBSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGWBSubsystem>();

            UGWBManager* Manager1 = Subsystem->GetManagerForWorld(World1);
            UGWBManager* Manager2 = Subsystem->GetManagerForWorld(World2);

            // Schedule work in both worlds
            Manager1->ScheduleWork("Default", FGWBWorkOptions());
            Manager2->ScheduleWork("Default", FGWBWorkOptions());

            uint32 World2CountBefore = Manager2->TotalWorkCount;

            // Destroy World1
            DestroyTestWorld(World1);

            // Verify World2 is unaffected
            TestEqual("World2 should be unaffected",
                      Manager2->TotalWorkCount, World2CountBefore);

            // Cleanup
            DestroyTestWorld(World2);
        });
    });
}
```

### Alternative Approaches Considered

#### Option A: UWorldSubsystem Instead of UEngineSubsystem

**Pros:**
- Native per-world lifecycle management
- Automatic cleanup on world destruction
- Simpler implementation

**Cons:**
- Breaking API change (different subsystem access pattern)
- Requires `UWorld::GetSubsystem<UGWBWorldSubsystem>()` instead of engine subsystem
- Configuration sharing across worlds becomes more complex

**Verdict:** Could be a future enhancement, but the map-based approach provides per-world isolation while maintaining API compatibility.

#### Option B: Hybrid Approach with Shared Configuration

Store shared configuration (WorkGroupDefinitions) at the subsystem level while maintaining per-world managers:

```cpp
UCLASS()
class UGWBSubsystem : public UEngineSubsystem
{
    // Shared across all worlds
    UPROPERTY(Config)
    TArray<FGWBWorkGroupDefinition> SharedWorkGroupDefinitions;

    // Per-world managers
    TMap<TWeakObjectPtr<UWorld>, UGWBManager*> WorldManagers;
};
```

This allows configuration to be defined once but have per-world execution state.

### Impact Analysis

| Component | Change Required | Complexity |
|-----------|----------------|------------|
| `UGWBSubsystem` | Major - TMap storage, new APIs | Medium |
| `UGWBManager` static APIs | Minor - Use context correctly | Low |
| `UGWBScheduler` | None - Already world-aware | None |
| Blueprint API | None - Already has WorldContext | None |
| Existing tests | Updates for new API | Low |
| New tests | Multi-world test cases | Medium |

### Rollout Plan

1. **Phase 1**: Implement per-world manager storage in `UGWBSubsystem`
2. **Phase 2**: Update static APIs to use world context correctly
3. **Phase 3**: Add deprecation warnings to `GetManager()`
4. **Phase 4**: Add multi-world tests
5. **Phase 5**: Update documentation and examples

### Summary

This proposal transforms the GWB system from a global singleton to a per-world instance model while maintaining backwards compatibility. The key changes are:

1. **Per-world manager storage** in `UGWBSubsystem` using `TMap<TWeakObjectPtr<UWorld>, UGWBManager*>`
2. **New world-aware API** via `GetManagerForWorld()` and `GetManagerForContext()`
3. **Fixed static API routing** to properly use `WorldContextObject` parameter
4. **Independent lifecycle management** - each world's manager is created/destroyed with its world

This ensures complete isolation between PIE instances and other multi-world scenarios while preserving the existing Blueprint and C++ APIs.
