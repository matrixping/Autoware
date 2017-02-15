/*
 *  Copyright (c) 2016, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "way_planner_core.h"

namespace WayPlannerNS {

void way_planner_core::GetTransformFromTF(const std::string parent_frame, const std::string child_frame, tf::StampedTransform &transform)
{
	static tf::TransformListener listener;

	int nFailedCounter = 0;
	while (1)
	{
		try
		{
			listener.lookupTransform(parent_frame, child_frame, ros::Time(0), transform);
			break;
		}
		catch (tf::TransformException& ex)
		{
			if(nFailedCounter > 2)
			{
				ROS_ERROR("%s", ex.what());
			}
			ros::Duration(1.0).sleep();
			nFailedCounter ++;
		}
	}
}

way_planner_core::way_planner_core()
{

	m_pCurrGoal = 0;
	m_iCurrentGoalIndex = -1;
	m_bKmlMap = false;
	bStartPos = false;
	bUsingCurrentPose = false;
	bEnableReplanning = false;
	m_ReplanDistanceFromEnd = 20; // meters
	nh.getParam("/way_planner/pathDensity" 			, m_params.pathDensity);
	nh.getParam("/way_planner/enableSmoothing" 		, m_params.bEnableSmoothing);
	nh.getParam("/way_planner/enableLaneChange" 	, m_params.bEnableLaneChange);
	nh.getParam("/way_planner/enableRvizInput" 		, m_params.bEnableRvizInput);
	nh.getParam("/way_planner/enableReplan" 		, bEnableReplanning);

	int iSource = 0;
	nh.getParam("/way_planner/mapSource" 			, iSource);
	if(iSource == 0)
		m_params.mapSource = MAP_AUTOWARE;
	else if (iSource == 1)
		m_params.mapSource = MAP_FOLDER;
	else if(iSource == 2)
		m_params.mapSource = MAP_KML_FILE;

	nh.getParam("/way_planner/mapFileName" 			, m_params.KmlMapPath);


	tf::StampedTransform transform;
	GetTransformFromTF("map", "world", transform);
	m_OriginPos.position.x  = transform.getOrigin().x();
	m_OriginPos.position.y  = transform.getOrigin().y();
	m_OriginPos.position.z  = transform.getOrigin().z();

	pub_Paths = nh.advertise<waypoint_follower::LaneArray>("lane_waypoints_array", 1, true);
	pub_PathsRviz = nh.advertise<visualization_msgs::MarkerArray>("global_waypoints_rviz", 1, true);
	pub_StartPointRviz = nh.advertise<visualization_msgs::Marker>("Global_StartPoint_rviz", 1, true);
	pub_GoalPointRviz = nh.advertise<visualization_msgs::MarkerArray>("Global_GoalPoints_rviz", 1, true);
	pub_NodesListRviz = nh.advertise<visualization_msgs::MarkerArray>("Goal_Nodes_Points_rviz", 1, true);
	pub_MapRviz  = nh.advertise<visualization_msgs::MarkerArray>("vector_map_center_lines_rviz", 100, true);
	pub_TrafficInfoRviz = nh.advertise<visualization_msgs::MarkerArray>("Traffic_Lights_rviz", 1, true);

#ifdef ENABLE_VISUALIZE_PLAN
	m_CurrMaxCost = 1;
	m_iCurrLevel = 0;
	m_nLevelSize = 1;
	m_bSwitch = 0;
	pub_GlobalPlanAnimationRviz = nh.advertise<visualization_msgs::MarkerArray>("AnimateGlobalPlan", 1, true);
#endif

#ifdef ENABLE_HMI
	m_AvgResponseTime = 3.5;
	m_SocketServer.InitSocket(10001);
#endif

	/** @todo To achieve perfection , you need to start sometime */

	//if(m_params.bEnableRvizInput)
	{
		sub_start_pose 	= nh.subscribe("/initialpose", 					1, &way_planner_core::callbackGetStartPose, 		this);
		sub_goal_pose 	= nh.subscribe("move_base_simple/goal", 		1, &way_planner_core::callbackGetGoalPose, 		this);
	}
//	else
//	{
//		sub_start_pose 	= nh.subscribe("/GlobalStartPose", 				1, &way_planner_core::callbackGetStartPose, 		this);
//		sub_goal_pose 	= nh.subscribe("/GlobalGoalPose", 				1, &way_planner_core::callbackGetGoalPose, 		this);
//	}

	sub_current_pose 		= nh.subscribe("/current_pose", 			100,	&way_planner_core::callbackGetCurrentPose, 		this);
	sub_current_velocity 	= nh.subscribe("/current_velocity",			100,	&way_planner_core::callbackGetVehicleStatus, 	this);
	sub_nodes_list 			= nh.subscribe("/GlobalNodesList", 			1, 		&way_planner_core::callbackGetNodesList, 		this);

	if(m_params.mapSource == MAP_AUTOWARE)
	{
		sub_map_points 	= nh.subscribe("/vector_map_info/point", 		1, &way_planner_core::callbackGetVMPoints, 		this);
		sub_map_lanes 	= nh.subscribe("/vector_map_info/lane", 		1, &way_planner_core::callbackGetVMLanes, 		this);
		sub_map_nodes 	= nh.subscribe("/vector_map_info/node", 		1, &way_planner_core::callbackGetVMNodes, 		this);
		sup_stop_lines 	= nh.subscribe("/vector_map_info/stop_line",	1, &way_planner_core::callbackGetVMStopLines, 	this);
		sub_dtlanes 	= nh.subscribe("/vector_map_info/dtlane", 		1, &way_planner_core::callbackGetVMCenterLines,	this);
	}
}

way_planner_core::~way_planner_core(){
}

void way_planner_core::callbackGetGoalPose(const geometry_msgs::PoseStampedConstPtr &msg)
{
	ROS_INFO("Received Goal Pose");
	m_GoalsPos.push_back(msg->pose);
}

void way_planner_core::callbackGetStartPose(const geometry_msgs::PoseWithCovarianceStampedConstPtr &msg)
{
	if(!bStartPos)
	{
		ROS_INFO("Received Start pose");
		m_StartPos = msg->pose.pose;
		bStartPos = true;
	}
}

void way_planner_core::callbackGetCurrentPose(const geometry_msgs::PoseStampedConstPtr& msg)
{
	//if(m_params.bEnableRvizInput)
	{
		m_CurrentPose = msg->pose;
		bUsingCurrentPose = true;
	}
}

void way_planner_core::callbackGetVehicleStatus(const geometry_msgs::TwistStampedConstPtr& msg)
{
	m_VehicleState.speed = msg->twist.linear.x;
	UtilityHNS::UtilityH::GetTickCount(m_VehicleState.tStamp);
}

void way_planner_core::callbackGetVMPoints(const vector_map_msgs::PointArray& msg)
{
	ROS_INFO("Received Map Points");
	m_AwMap.points = msg;
	m_AwMap.bPoints = true;
}

void way_planner_core::callbackGetVMLanes(const vector_map_msgs::LaneArray& msg)
{
	ROS_INFO("Received Map Lane Array");
	m_AwMap.lanes = msg;
	m_AwMap.bLanes = true;
}

void way_planner_core::callbackGetVMNodes(const vector_map_msgs::NodeArray& msg)
{
	//ROS_INFO("Received Map Nodes");


}

void way_planner_core::callbackGetVMStopLines(const vector_map_msgs::StopLineArray& msg)
{
	//ROS_INFO("Received Map Stop Lines");
}

void way_planner_core::callbackGetVMCenterLines(const vector_map_msgs::DTLaneArray& msg)
{
	ROS_INFO("Received Map Center Lines");
	m_AwMap.dtlanes = msg;
	m_AwMap.bDtLanes = true;
}

void way_planner_core::callbackGetNodesList(const vector_map_msgs::NodeArray& msg)
{

}

void way_planner_core::UpdateRoadMap(const AutowareRoadNetwork& src_map, PlannerHNS::RoadNetwork& out_map)
{
	std::vector<UtilityHNS::AisanLanesFileReader::AisanLane> lanes;
	for(unsigned int i=0; i < src_map.lanes.data.size();i++)
	{
		UtilityHNS::AisanLanesFileReader::AisanLane l;
		l.BLID 		=  src_map.lanes.data.at(i).blid;
		l.BLID2 	=  src_map.lanes.data.at(i).blid2;
		l.BLID3 	=  src_map.lanes.data.at(i).blid3;
		l.BLID4 	=  src_map.lanes.data.at(i).blid4;
		l.BNID 		=  src_map.lanes.data.at(i).bnid;
		l.ClossID 	=  src_map.lanes.data.at(i).clossid;
		l.DID 		=  src_map.lanes.data.at(i).did;
		l.FLID 		=  src_map.lanes.data.at(i).flid;
		l.FLID2 	=  src_map.lanes.data.at(i).flid2;
		l.FLID3 	=  src_map.lanes.data.at(i).flid3;
		l.FLID4 	=  src_map.lanes.data.at(i).flid4;
		l.FNID 		=  src_map.lanes.data.at(i).fnid;
		l.JCT 		=  src_map.lanes.data.at(i).jct;
		l.LCnt 		=  src_map.lanes.data.at(i).lcnt;
		l.LnID 		=  src_map.lanes.data.at(i).lnid;
		l.Lno 		=  src_map.lanes.data.at(i).lno;
		l.Span 		=  src_map.lanes.data.at(i).span;
		l.RefVel	=  src_map.lanes.data.at(i).refvel;
		l.LimitVel	=  src_map.lanes.data.at(i).limitvel;

//		l.LaneChgFG =  src_map.lanes.at(i).;
//		l.LaneType 	=  src_map.lanes.at(i).blid;
//		l.LimitVel 	=  src_map.lanes.at(i).;
//		l.LinkWAID 	=  src_map.lanes.at(i).blid;
//		l.RefVel 	=  src_map.lanes.at(i).blid;
//		l.RoadSecID =  src_map.lanes.at(i).;

		lanes.push_back(l);
	}

	std::vector<UtilityHNS::AisanPointsFileReader::AisanPoints> points;

	for(unsigned int i=0; i < src_map.points.data.size();i++)
	{
		UtilityHNS::AisanPointsFileReader::AisanPoints p;
		double integ_part = src_map.points.data.at(i).l;
		double deg = trunc(src_map.points.data.at(i).l);
		double min = trunc((src_map.points.data.at(i).l - deg) * 100.0) / 60.0;
		double sec = modf((src_map.points.data.at(i).l - deg) * 100.0, &integ_part)/36.0;
		double L =  deg + min + sec;

		deg = trunc(src_map.points.data.at(i).b);
		min = trunc((src_map.points.data.at(i).b - deg) * 100.0) / 60.0;
		sec = modf((src_map.points.data.at(i).b - deg) * 100.0, &integ_part)/36.0;
		double B =  deg + min + sec;

		p.B 		= B;
		p.Bx 		= src_map.points.data.at(i).bx;
		p.H 		= src_map.points.data.at(i).h;
		p.L 		= L;
		p.Ly 		= src_map.points.data.at(i).ly;
		p.MCODE1 	= src_map.points.data.at(i).mcode1;
		p.MCODE2 	= src_map.points.data.at(i).mcode2;
		p.MCODE3 	= src_map.points.data.at(i).mcode3;
		p.PID 		= src_map.points.data.at(i).pid;
		p.Ref 		= src_map.points.data.at(i).ref;

		points.push_back(p);
	}


	std::vector<UtilityHNS::AisanCenterLinesFileReader::AisanCenterLine> dts;
	for(unsigned int i=0; i < src_map.dtlanes.data.size();i++)
	{
		UtilityHNS::AisanCenterLinesFileReader::AisanCenterLine dt;

		dt.Apara 	= src_map.dtlanes.data.at(i).apara;
		dt.DID 		= src_map.dtlanes.data.at(i).did;
		dt.Dir 		= src_map.dtlanes.data.at(i).dir;
		dt.Dist 	= src_map.dtlanes.data.at(i).dist;
		dt.LW 		= src_map.dtlanes.data.at(i).lw;
		dt.PID 		= src_map.dtlanes.data.at(i).pid;
		dt.RW 		= src_map.dtlanes.data.at(i).rw;
		dt.cant 	= src_map.dtlanes.data.at(i).cant;
		dt.r 		= src_map.dtlanes.data.at(i).r;
		dt.slope 	= src_map.dtlanes.data.at(i).slope;

		dts.push_back(dt);
	}

	std::vector<UtilityHNS::AisanAreasFileReader::AisanArea> areas;
	std::vector<UtilityHNS::AisanIntersectionFileReader::AisanIntersection> inters;
	std::vector<UtilityHNS::AisanLinesFileReader::AisanLine> line_data;
	std::vector<UtilityHNS::AisanStopLineFileReader::AisanStopLine> stop_line_data;
	std::vector<UtilityHNS::AisanSignalFileReader::AisanSignal> signal_data;
	std::vector<UtilityHNS::AisanVectorFileReader::AisanVector> vector_data;
	std::vector<UtilityHNS::AisanDataConnFileReader::DataConn> conn_data;

	PlannerHNS::GPSPoint origin;//(m_OriginPos.position.x, m_OriginPos.position.y, m_OriginPos.position.z, 0);
	PlannerHNS::MappingHelpers::ConstructRoadNetworkFromRosMessage(lanes, points, dts,inters, areas, line_data, stop_line_data, signal_data, vector_data,conn_data, origin, out_map);
}

bool way_planner_core::GenerateGlobalPlan(PlannerHNS::WayPoint& startPoint, PlannerHNS::WayPoint& goalPoint, std::vector<std::vector<PlannerHNS::WayPoint> >& generatedTotalPaths)
{
	PlannerHNS::WayPoint* pStart = PlannerHNS::MappingHelpers::GetClosestWaypointFromMap(startPoint, m_Map);
	PlannerHNS::WayPoint* pGoal = PlannerHNS::MappingHelpers::GetClosestWaypointFromMap(goalPoint, m_Map);
	std::vector<int> predefinedLanesIds;

	if(pStart && pGoal)
	{
		generatedTotalPaths.clear();
#ifdef ENABLE_VISUALIZE_PLAN
		if(m_PlanningVisualizeTree.size() > 0)
		{
			m_PlannerH.DeleteWaypoints(m_PlanningVisualizeTree);
			m_AccumPlanLevels.markers.clear();
			m_iCurrLevel = 0;
			m_nLevelSize = 1;
		}

		double ret = m_PlannerH.PlanUsingDP(pStart->pLane,
				*pStart, *pGoal,
				*pStart, MAX_GLOBAL_PLAN_DISTANCE,
				predefinedLanesIds, generatedTotalPaths, &m_PlanningVisualizeTree);

		m_pCurrGoal = pGoal;

#else
		double ret = m_PlannerH.PlanUsingDP(pStart->pLane,
						*pStart, *pGoal,
						*pStart, MAX_GLOBAL_PLAN_DISTANCE,
						predefinedLanesIds, generatedTotalPaths);
#endif

		if(ret == 0) generatedTotalPaths.clear();

		if(generatedTotalPaths.size() > 0 && generatedTotalPaths.at(0).size()>0)
		{
			if(m_params.bEnableSmoothing)
			{
				for(unsigned int i=0; i < generatedTotalPaths.size(); i++)
				{
					PlannerHNS::PlanningHelpers::FixPathDensity(generatedTotalPaths.at(i), m_params.pathDensity);
					PlannerHNS::PlanningHelpers::SmoothPath(generatedTotalPaths.at(i), 0.49, 0.35 , 0.01);
					PlannerHNS::PlanningHelpers::CalcAngleAndCost(generatedTotalPaths.at(i));
				}
			}

			std::cout << "New DP Path -> " << generatedTotalPaths.size() << std::endl;
			return true;
		}
		else
		{
			std::cout << "Can't Generate Global Path for Start (" << startPoint.pos.ToString()
								<< ") and Goal (" << goalPoint.pos.ToString() << ")" << std::endl;
		}
	}
	else
	{
		std::cout << "Can't Find Global Waypoint Nodes in the Map for Start (" << startPoint.pos.ToString()
							<< ") and Goal (" << goalPoint.pos.ToString() << ")" << std::endl;
	}

	return false;
}

void way_planner_core::VisualizeAndSend(const std::vector<std::vector<PlannerHNS::WayPoint> > generatedTotalPaths)
{
	waypoint_follower::LaneArray lane_array;
	visualization_msgs::MarkerArray pathsToVisualize;

	for(unsigned int i=0; i < generatedTotalPaths.size(); i++)
		RosHelpers::ConvertFromPlannerHToAutowarePathFormat(generatedTotalPaths.at(i), lane_array);

	std_msgs::ColorRGBA total_color;
	total_color.r = 0;
	total_color.g = 0.7;
	total_color.b = 1.0;
	total_color.a = 0.4;
	RosHelpers::createGlobalLaneArrayMarker(total_color, lane_array, pathsToVisualize);

	RosHelpers::createGlobalLaneArrayOrientationMarker(lane_array, pathsToVisualize);
	RosHelpers::createGlobalLaneArrayVelocityMarker(lane_array, pathsToVisualize);
	//RosHelpers::ConvertFromPlannerHToAutowareVisualizePathFormat(generatedTotalPaths, pathsToVisualize);
	pub_PathsRviz.publish(pathsToVisualize);
	pub_Paths.publish(lane_array);

	for(unsigned int i=0; i < generatedTotalPaths.size(); i++)
	{
		std::ostringstream str_out;
		str_out << UtilityHNS::UtilityH::GetHomeDirectory();
		str_out << UtilityHNS::DataRW::LoggingMainfolderName;
		str_out << "GlobalPath_";
		str_out << i;
		str_out << "_";
		PlannerHNS::PlanningHelpers::WritePathToFile(str_out.str(), generatedTotalPaths.at(i));
	}
}

#ifdef ENABLE_VISUALIZE_PLAN
void way_planner_core::CreateNextPlanningTreeLevelMarker(std::vector<PlannerHNS::WayPoint*>& level, visualization_msgs::MarkerArray& markerArray, double max_cost)
{
	if(level.size() == 0 && m_pCurrGoal)
		return;

	std::vector<PlannerHNS::WayPoint*> newlevel;

	//lane_waypoint_marker.frame_locked = false;

	for(unsigned int i = 0; i < level.size(); i++)
	{
		visualization_msgs::Marker lane_waypoint_marker;
		lane_waypoint_marker.header.frame_id = "map";
		lane_waypoint_marker.header.stamp = ros::Time();
		lane_waypoint_marker.type = visualization_msgs::Marker::ARROW;
		lane_waypoint_marker.ns = "tree_levels";
		lane_waypoint_marker.action = visualization_msgs::Marker::ADD;
		lane_waypoint_marker.scale.x = 1.0;
		lane_waypoint_marker.scale.y = 0.5;
		lane_waypoint_marker.scale.z = 0.5;
		lane_waypoint_marker.color.a = 0.8;
		lane_waypoint_marker.color.b = 1-0.0;

		float norm_cost = level.at(i)->cost / max_cost * 2.0;
		if(norm_cost <= 1.0)
		{
			lane_waypoint_marker.color.r = 1-norm_cost;
			lane_waypoint_marker.color.g = 1-1.0;
		}
		else if(norm_cost > 1.0)
		{
			lane_waypoint_marker.color.r = 1-1.0;
			lane_waypoint_marker.color.g = 1- (2.0 - norm_cost);
		}

		if(markerArray.markers.size() == 0)
			lane_waypoint_marker.id = 0;
		else
			lane_waypoint_marker.id = markerArray.markers.at(markerArray.markers.size()-1).id + 1;

		lane_waypoint_marker.pose.position.x = level.at(i)->pos.x;
		lane_waypoint_marker.pose.position.y = level.at(i)->pos.y;
		lane_waypoint_marker.pose.position.z = level.at(i)->pos.z;
		double a = UtilityHNS::UtilityH::SplitPositiveAngle(level.at(i)->pos.a);
		lane_waypoint_marker.pose.orientation = tf::createQuaternionMsgFromYaw(a);
		markerArray.markers.push_back(lane_waypoint_marker);

		if(level.at(i)->pLeft)
		{
			lane_waypoint_marker.pose.orientation = tf::createQuaternionMsgFromYaw(a + M_PI_2);
			newlevel.push_back(level.at(i)->pLeft);
			lane_waypoint_marker.id = markerArray.markers.at(markerArray.markers.size()-1).id + 1;
			markerArray.markers.push_back(lane_waypoint_marker);
		}
		if(level.at(i)->pRight)
		{
			newlevel.push_back(level.at(i)->pRight);
			lane_waypoint_marker.pose.orientation = tf::createQuaternionMsgFromYaw(a - M_PI_2);
			lane_waypoint_marker.id = markerArray.markers.at(markerArray.markers.size()-1).id + 1;
			markerArray.markers.push_back(lane_waypoint_marker);
		}

		for(unsigned int j = 0; j < level.at(i)->pFronts.size(); j++)
			if(level.at(i)->pFronts.at(j))
				newlevel.push_back(level.at(i)->pFronts.at(j));

		if(hypot(m_pCurrGoal->pos.y - level.at(i)->pos.y, m_pCurrGoal->pos.x - level.at(i)->pos.x) < 0.5)
		{
			newlevel.clear();
			break;
		}

		std::cout << "Levels: " <<  lane_waypoint_marker.id << ", pLeft:" << level.at(i)->pLeft << ", pRight:" << level.at(i)->pRight << ", nFront:" << level.at(i)->pFronts.size() << ", Cost: "<< norm_cost<< std::endl;
	}

	level = newlevel;

	//std::cout << "Levels: " <<  level.size() << std::endl;
}

#endif

#ifdef ENABLE_HMI
  	void way_planner_core::HMI_DoOneStep()
  	{
  		double min_distance = m_AvgResponseTime * m_VehicleState.speed;
  		std::vector<PlannerHNS::WayPoint> branches;

  		PlannerHNS::WayPoint startPoint;

  		if(bStartPos || bUsingCurrentPose)
  		{
			if(bUsingCurrentPose)
			{
				startPoint = PlannerHNS::WayPoint(m_CurrentPose.position.x,
						m_CurrentPose.position.y,
						m_CurrentPose.position.z, tf::getYaw(m_CurrentPose.orientation));
			}
			else
			{
				startPoint = PlannerHNS::WayPoint(m_StartPos.position.x+m_OriginPos.position.x,
						m_StartPos.position.y+m_OriginPos.position.y,
						m_StartPos.position.z+m_OriginPos.position.z, tf::getYaw(m_StartPos.orientation));
			}
  		}

  		RosHelpers::FindIncommingBranches(m_GeneratedTotalPaths,startPoint, min_distance, branches);
  		if(branches.size() > 0)
  		{
			HMI_MSG msg;
			msg.type = OPTIONS_MSG;
			msg.options.clear();
			for(unsigned int i = 0; i< branches.size(); i++)
				msg.options.push_back(branches.at(i).actionCost.at(0).first);

			std::cout << "Send Message (" <<  branches.size() << ") Branches (" ;
			for(unsigned int i=0; i< branches.size(); i++)
			{
				if(branches.at(i).actionCost.at(0).first == PlannerHNS::FORWARD_ACTION)
					std::cout << "F,";
				else if(branches.at(i).actionCost.at(0).first == PlannerHNS::LEFT_TURN_ACTION)
					std::cout << "L,";
				else if(branches.at(i).actionCost.at(0).first == PlannerHNS::RIGHT_TURN_ACTION)
					std::cout << "R,";
			}
			std::cout << ")" << std::endl;
			m_SocketServer.SendMSG(msg);
  		}
  	}
#endif

void way_planner_core::PlannerMainLoop()
{
	ros::Rate loop_rate(10);
	timespec animation_timer;
	UtilityHNS::UtilityH::GetTickCount(animation_timer);

	while (ros::ok())
	{
		ros::spinOnce();
#ifdef ENABLE_HMI
		HMI_DoOneStep();
#endif

		//std::cout << "Main Loop ! " << std::endl;
		if(m_params.mapSource == MAP_KML_FILE && !m_bKmlMap)
		{
			m_bKmlMap = true;
			PlannerHNS::MappingHelpers::LoadKML(m_params.KmlMapPath, m_Map);
			//PlannerHNS::MappingHelpers::WriteKML("/home/user/SimuLogs/KmlMaps/HalfNicMap.kml", "/home/user/SimuLogs/KmlMaps/PlannerX_MapTemplate.kml", m_Map);
			visualization_msgs::MarkerArray map_marker_array;
			RosHelpers::ConvertFromRoadNetworkToAutowareVisualizeMapFormat(m_Map, map_marker_array);

			pub_MapRviz.publish(map_marker_array);
		}
		else if (m_params.mapSource == MAP_FOLDER && !m_bKmlMap)
		{
			m_bKmlMap = true;
			PlannerHNS::MappingHelpers::ConstructRoadNetworkFromDataFiles(m_params.KmlMapPath, m_Map, true);
//			PlannerHNS::MappingHelpers::WriteKML("/home/user/SimuLogs/KmlMaps/ToyotaCity_NoTransform.kml", "/home/user/SimuLogs/KmlMaps/PlannerX_MapTemplate.kml", m_Map);
			visualization_msgs::MarkerArray map_marker_array;
			RosHelpers::ConvertFromRoadNetworkToAutowareVisualizeMapFormat(m_Map, map_marker_array);

			pub_MapRviz.publish(map_marker_array);

		}
		else if(m_params.mapSource == MAP_AUTOWARE)
		{
			 if(m_AwMap.bDtLanes && m_AwMap.bLanes && m_AwMap.bPoints)
			 {
				 m_AwMap.bDtLanes = m_AwMap.bLanes = m_AwMap.bPoints = false;
				 UpdateRoadMap(m_AwMap,m_Map);
				visualization_msgs::MarkerArray map_marker_array;
				RosHelpers::ConvertFromRoadNetworkToAutowareVisualizeMapFormat(m_Map, map_marker_array);
				pub_MapRviz.publish(map_marker_array);
			 }
		}

		if(bStartPos || bUsingCurrentPose)
		{
			PlannerHNS::WayPoint startPoint;
			PlannerHNS::WayPoint goalPoint;

			if(bUsingCurrentPose)
			{
				startPoint = PlannerHNS::WayPoint(m_CurrentPose.position.x,
						m_CurrentPose.position.y,
						m_CurrentPose.position.z, tf::getYaw(m_CurrentPose.orientation));
			}
			else
			{
				startPoint = PlannerHNS::WayPoint(m_StartPos.position.x+m_OriginPos.position.x,
						m_StartPos.position.y+m_OriginPos.position.y,
						m_StartPos.position.z+m_OriginPos.position.z, tf::getYaw(m_StartPos.orientation));
			}

			bool bMakeNewPlan = false;
			if(bEnableReplanning)
			{
				if(m_GeneratedTotalPaths.size() > 0 && m_GeneratedTotalPaths.at(0).size() > 3)
				{
					double calcDensity = hypot(m_GeneratedTotalPaths.at(0).at(1).pos.y - m_GeneratedTotalPaths.at(0).at(0).pos.y,
							m_GeneratedTotalPaths.at(0).at(1).pos.x - m_GeneratedTotalPaths.at(0).at(0).pos.x);

					if(calcDensity < 0.1)
						calcDensity = hypot(m_GeneratedTotalPaths.at(0).at(2).pos.y - m_GeneratedTotalPaths.at(0).at(1).pos.y,
										m_GeneratedTotalPaths.at(0).at(2).pos.x - m_GeneratedTotalPaths.at(0).at(1).pos.x);

					if(calcDensity == 0)
						calcDensity = 0.25;

					int nReplanIndex = m_ReplanDistanceFromEnd / calcDensity;
					int nToEndPoints = m_GeneratedTotalPaths.at(0).size() - PlannerHNS::PlanningHelpers::GetClosestPointIndex(m_GeneratedTotalPaths.at(0), startPoint);
					if(nToEndPoints <= nReplanIndex)
					{
						m_iCurrentGoalIndex = m_iCurrentGoalIndex + 1;
						bMakeNewPlan = true;
					}
				}
				else if(m_GoalsPos.size() > 0)
				{
					m_iCurrentGoalIndex = 0;
					bMakeNewPlan = true;
				}
			}
			else
			{
				if(m_GeneratedTotalPaths.size() == 0 && m_GoalsPos.size() > 0)
				{
					m_iCurrentGoalIndex = 0;
					bMakeNewPlan = true;
				}
			}

			if(bMakeNewPlan)
			{
				int gIndex = m_iCurrentGoalIndex%m_GoalsPos.size();
				if( gIndex < (int)m_GoalsPos.size())
				{
					goalPoint = PlannerHNS::WayPoint(m_GoalsPos.at(gIndex).position.x+m_OriginPos.position.x,
										m_GoalsPos.at(gIndex).position.y+m_OriginPos.position.y,
										m_GoalsPos.at(gIndex).position.z+m_OriginPos.position.z,
										tf::getYaw(m_GoalsPos.at(gIndex).orientation));

					bool bNewPlan = GenerateGlobalPlan(startPoint, goalPoint, m_GeneratedTotalPaths);

					if(bNewPlan)
					{
						VisualizeAndSend(m_GeneratedTotalPaths);
#ifdef ENABLE_VISUALIZE_PLAN
						//calculate new max_cost
						if(m_PlanningVisualizeTree.size() > 1)
						{
							m_CurrentLevel.push_back(m_PlanningVisualizeTree.at(0));
							m_CurrMaxCost = 0;
							for(unsigned int itree = 0; itree < m_PlanningVisualizeTree.size(); itree++)
							{
								if(m_PlanningVisualizeTree.at(itree)->cost > m_CurrMaxCost)
									m_CurrMaxCost = m_PlanningVisualizeTree.at(itree)->cost;
							}
						}
#endif
					}
				}
			}

#ifdef ENABLE_VISUALIZE_PLAN
			if(UtilityHNS::UtilityH::GetTimeDiffNow(animation_timer) > 0.5)
			{
				UtilityHNS::UtilityH::GetTickCount(animation_timer);
				m_CurrentLevel.clear();

				for(unsigned int ilev = 0; ilev < m_nLevelSize && m_iCurrLevel < m_PlanningVisualizeTree.size() ; ilev ++)
				{
					m_CurrentLevel.push_back(m_PlanningVisualizeTree.at(m_iCurrLevel));
					m_nLevelSize += m_PlanningVisualizeTree.at(m_iCurrLevel)->pFronts.size() - 1;
					m_iCurrLevel++;
				}


				if(m_CurrentLevel.size() == 0 && m_GeneratedTotalPaths.size() > 0)
				{
					m_bSwitch++;
					m_AccumPlanLevels.markers.clear();

					if(m_bSwitch == 2)
					{
						for(unsigned int il = 0; il < m_GeneratedTotalPaths.size(); il++)
							for(unsigned int ip = 0; ip < m_GeneratedTotalPaths.at(il).size(); ip ++)
								m_CurrentLevel.push_back(&m_GeneratedTotalPaths.at(il).at(ip));

						std::cout << "Switch On " << std::endl;

						m_bSwitch = 0;

					}
					else
					{
						for(unsigned int ilev = 0; ilev < m_PlanningVisualizeTree.size()+200 ; ilev ++)
							m_CurrentLevel.push_back(m_PlanningVisualizeTree.at(0));

						std::cout << "Switch Off " << std::endl;
					}

					CreateNextPlanningTreeLevelMarker(m_CurrentLevel, m_AccumPlanLevels, m_CurrMaxCost);
					pub_GlobalPlanAnimationRviz.publish(m_AccumPlanLevels);
				}
				else
				{
					CreateNextPlanningTreeLevelMarker(m_CurrentLevel, m_AccumPlanLevels, m_CurrMaxCost);

					if(m_AccumPlanLevels.markers.size() > 0)
						pub_GlobalPlanAnimationRviz.publish(m_AccumPlanLevels);
				}
			}
#endif

		}

		//ROS_INFO("Main Loop Step");
		loop_rate.sleep();
	}
}

}
