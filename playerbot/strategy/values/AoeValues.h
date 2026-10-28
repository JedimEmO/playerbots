#pragma once
#include "playerbot/strategy/Value.h"

namespace ai
{
    class AoePositionValue : public CalculatedValue<WorldLocation>
	{
	public:
        AoePositionValue(PlayerbotAI* ai) : CalculatedValue<WorldLocation>(ai, "aoe position") {}

	public:
        virtual WorldLocation Calculate();
    };

    class AoeCountValue : public CalculatedValue<uint8>
	{
	public:
        AoeCountValue(PlayerbotAI* ai) : CalculatedValue<uint8>(ai, "aoe count") {}

	public:
        static std::list<ObjectGuid> FindMaxDensity(Player* bot, float range = 100.0f);
        virtual uint8 Calculate() override;
    };

    class HasAreaDebuffValue : public BoolCalculatedValue, public Qualified
    {
    public:
        HasAreaDebuffValue(PlayerbotAI* ai) : BoolCalculatedValue(ai), Qualified() {}

        Unit* GetTarget()
        {
            AiObjectContext* ctx = AiObject::context;

            // A qualifier that names no value, or none, has no target.
            Value<ObjectGuid>* target = ctx->GetValue<ObjectGuid>(qualifier);
            return target ? ai->GetUnit(target->Get()) : nullptr;
        }
        virtual bool Calculate() override;
    };
}
