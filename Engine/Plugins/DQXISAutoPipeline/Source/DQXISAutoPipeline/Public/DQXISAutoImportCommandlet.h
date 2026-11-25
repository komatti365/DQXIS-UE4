#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DQXISAutoImportCommandlet.generated.h"

UCLASS()
class UDQXISAutoImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDQXISAutoImportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
