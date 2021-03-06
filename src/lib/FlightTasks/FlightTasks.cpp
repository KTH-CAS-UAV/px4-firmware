#include "FlightTasks.hpp"

#include <uORB/topics/vehicle_command.h>
#include <uORB/topics/vehicle_command_ack.h>

FlightTasks::FlightTasks()
{
	// initialize all flight-tasks
	// currently this is required to get all parameters read
	for (int i = 0; i < static_cast<int>(FlightTaskIndex::Count); i++) {
		_initTask(static_cast<FlightTaskIndex>(i));
	}

	// disable all tasks
	_initTask(FlightTaskIndex::None);
}

Error FlightTasks::update()
{
    Error error;
	_updateCommand();

	if (!isAnyTaskActive()) {
        return "no active task";
    }

	_subscription_array.update();
	if (!(error = _current_task.task->updateInitialize()).ok()) {
        return error;
    }
    if (!(error = _current_task.task->update()).ok()) {
        return error;
	}

	return true;
}

const vehicle_local_position_setpoint_s FlightTasks::getPositionSetpoint()
{
	if (isAnyTaskActive()) {
		return _current_task.task->getPositionSetpoint();

	} else {
		return FlightTask::empty_setpoint;
	}
}

const vehicle_constraints_s FlightTasks::getConstraints()
{
	if (isAnyTaskActive()) {
		return _current_task.task->getConstraints();

	} else {
		return FlightTask::empty_constraints;
	}
}

const vehicle_trajectory_waypoint_s FlightTasks::getAvoidanceWaypoint()
{
	if (isAnyTaskActive()) {
		return _current_task.task->getAvoidanceWaypoint();

	} else {
		return FlightTask::empty_trajectory_waypoint;
	}
}

const vehicle_trajectory_waypoint_s &FlightTasks::getEmptyAvoidanceWaypoint()
{
	return FlightTask::empty_trajectory_waypoint;
}

Error FlightTasks::switchTask(FlightTaskIndex new_task_index)
{
    Error error;

	// switch to the running task, nothing to do
	if (new_task_index == _current_task.index) {
		return {};
	}

	if (_initTask(new_task_index)) {
		// invalid task
		return {-1, "invalid task"};
	}

	if (!_current_task.task) {
		// no task running
		return {};
	}

	// subscription failed
	error = _current_task.task->initializeSubscriptions(_subscription_array);
	if (!error.ok()) {
		_current_task.task->~FlightTask();
		_current_task.task = nullptr;
		_current_task.index = FlightTaskIndex::None;
		return error;
	}

	_subscription_array.forcedUpdate(); // make sure data is available for all new subscriptions

	// update initialize failed
	error = _current_task.task->updateInitialize();
	if (!error.ok()) {
		_current_task.task->~FlightTask();
		_current_task.task = nullptr;
		_current_task.index = FlightTaskIndex::None;
		return error;
	}

	// activation failed
	error = _current_task.task->activate();
	if (!error.ok()) {
		_current_task.task->~FlightTask();
		_current_task.task = nullptr;
		_current_task.index = FlightTaskIndex::None;
		return error;
	}

	return true;
}

Error FlightTasks::switchTask(int new_task_index)
{
	// make sure we are in range of the enumeration before casting
	if (static_cast<int>(FlightTaskIndex::None) <= new_task_index &&
	    static_cast<int>(FlightTaskIndex::Count) > new_task_index) {
		return switchTask(FlightTaskIndex(new_task_index));
	}

	switchTask(FlightTaskIndex::None);
	return "invalid task";
}

void FlightTasks::handleParameterUpdate()
{
	if (isAnyTaskActive()) {
		_current_task.task->handleParameterUpdate();
	}
}

const char *FlightTasks::errorToString(const int error)
{
	for (auto e : _errors) {
		if (e.num() == error) {
			return e.msg();
		}
	}

	return "This error is not mapped to a string or is unknown.";
}

void FlightTasks::_updateCommand()
{
	// lazy subscription to command topic
	if (_sub_vehicle_command < 0) {
		_sub_vehicle_command = orb_subscribe(ORB_ID(vehicle_command));
	}

	// check if there's any new command
	bool updated = false;
	orb_check(_sub_vehicle_command, &updated);

	if (!updated) {
		return;
	}

	// get command
	struct vehicle_command_s command;
	orb_copy(ORB_ID(vehicle_command), _sub_vehicle_command, &command);

	// check what command it is
	FlightTaskIndex desired_task = switchVehicleCommand(command.command);

	if (desired_task == FlightTaskIndex::None) {
		// ignore all unkown commands
		return;
	}

	// switch to the commanded task
	Error switch_result = switchTask(desired_task);
	uint8_t cmd_result = vehicle_command_ack_s::VEHICLE_RESULT_FAILED;

	// if we are in/switched to the desired task
	if (!switch_result.ok()) {
		cmd_result = vehicle_command_ack_s::VEHICLE_RESULT_ACCEPTED;

		// if the task is running apply parameters to it and see if it rejects
		if (isAnyTaskActive() && !_current_task.task->applyCommandParameters(command).ok()) {
			cmd_result = vehicle_command_ack_s::VEHICLE_RESULT_DENIED;

			// if we just switched and parameters are not accepted, go to failsafe
			if (switch_result.num() == 1) {
				switchTask(FlightTaskIndex::ManualPosition);
				cmd_result = vehicle_command_ack_s::VEHICLE_RESULT_FAILED;
			}
		}
	}

	// send back acknowledgment
	vehicle_command_ack_s command_ack = {};
	command_ack.command = command.command;
	command_ack.result = cmd_result;
	command_ack.result_param1 = switch_result.num();
	command_ack.target_system = command.source_system;
	command_ack.target_component = command.source_component;

	if (_pub_vehicle_command_ack == nullptr) {
		_pub_vehicle_command_ack = orb_advertise_queue(ORB_ID(vehicle_command_ack), &command_ack,
					   vehicle_command_ack_s::ORB_QUEUE_LENGTH);

	} else {
		orb_publish(ORB_ID(vehicle_command_ack), _pub_vehicle_command_ack, &command_ack);

	}
}
