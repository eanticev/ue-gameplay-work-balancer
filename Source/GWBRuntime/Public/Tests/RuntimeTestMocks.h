﻿

#pragma once

#include "CoreMinimal.h"
#include "GWBManager.h"
#include "DataTypes/GWBWorkUnitHandle.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGWBTests, Log, All);

#include "RuntimeTestMocks.generated.h"

#pragma region Test Helpers

/**
 * MOCK for Manager that provides test fixtures
 */
UCLASS()
class UGWBManagerMock : public UGWBManager
{
	GENERATED_BODY()
public:
	UGWBScheduler* TEST_GetScheduler() { return Scheduler.Get(); };
	FGWBWorkUnitHandle TEST_ScheduleWork(const FName& WorkGroupId, const FGWBWorkOptions& WorkOptions) { return ScheduleWork(WorkGroupId, WorkOptions); }
	void TEST_DoWork() { DoWork(); };
	uint32 TEST_GetWorkUnitCount()
	{
		int32 Count = 0;
		for (auto G : WorkGroups)
		{
			for (auto U : G.WorkUnitsQueue)
			{
				Count++;
			}
		}
		return Count;
	};
	void FakeInitialize()
	{
		Scheduler = NewObject<UGWBScheduler>();
	};
};

/**
 * Wraps a LAMBDA for sake of binding to a dynamic multicast delegate
 */
UCLASS()
class ULambdaWrapper : public UObject
{
	GENERATED_BODY()
public:
	TFunction<void(float DeltaTime)> CallFn;
	UFUNCTION()
	void Dispatch(float DeltaTime) { CallFn(DeltaTime); }
};

/**
 * Test Helpers
 */
struct FGWBManagerTestHelper
{
	static UGWBManagerMock* Create(UObject* Outer = nullptr)
	{
		if (!Outer)
		{
			Outer = GetTransientPackage();
		}
		UGWBManagerMock* Manager = NewObject<UGWBManagerMock>(Outer);
		check(Manager);
		Manager->FakeInitialize();
		return Manager;
	}

	// static FGWBHandleWorkDelegate MakeHandleWorkCallback(TFunction<void(float DeltaTime)> FnIn)
	// {
	// 	const auto Wrapper = NewObject<ULambdaWrapper>();
	// 	Wrapper->CallFn = FnIn;
	// 	FGWBHandleWorkDelegate Callback;
	// 	Callback.AddDynamic(Wrapper, &ULambdaWrapper::Dispatch);
	// 	return Callback;
	// }
};

#pragma endregion Test Helpers