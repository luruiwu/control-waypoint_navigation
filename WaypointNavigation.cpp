#include "WaypointNavigation.hpp"
#include <iostream>

namespace waypoint_navigation_lib{

WaypointNavigation::WaypointNavigation()
{
    mNavigationState = TARGET_REACHED;
    // Booleans
    aligning    = false;
    targetSet   = false;
    poseSet     = false;
    newWaypoint = false;
    finalPhase  = false;

    // Ackermann turn parameters
    minTurnRadius    = 0.6; // (in meters)
    maxDisplacementAckermannTurn = 0.25; // (meters from straight line to the next point)
    // Alignment parameters
    maxDisalignment = 5.0 / 180.0 * M_PI;

    // Velocities
    translationalVelocity = 0.05; // [m/s]
    rotationalVelocity = 0.15;    // [rad/s] ... cca 8.6 deg/s
    // Distances
    corridor = .2;
    lookaheadDistance = .6;
    distanceToNext = new std::vector<double>();

}

NavigationState WaypointNavigation::getNavigationState() {
    return mNavigationState;
}

void  WaypointNavigation::setNavigationState(NavigationState state){
    mNavigationState = state;
}
double WaypointNavigation::getLookaheadDistance(){
    return lookaheadDistance;
}
bool WaypointNavigation::setPose(base::samples::RigidBodyState& pose)
{
    if( isnan(pose.position(0)) || isnan(pose.position(1)) ){
        // Position data are not valid, pose will not be set
        std::cout << "Position is not valid!" << std::endl;
        return false;
    } else {
        curPose = pose;
        xr = base::Vector2d(pose.position(0), pose.position(1));
        if (!poseSet && !trajectory.empty()){
            w1 << curPose.position(0), curPose.position(1);
            setSegmentWaypoint(w2, currentSegment);
        }
        poseSet=true;
        return true;
    }
}

void WaypointNavigation::setLookaheadPoint(base::Waypoint& waypoint)
{
    targetSet = true;
    aligning = false;
    lookaheadPoint = waypoint;
    newWaypoint = true;
}

// Implementation of "pure pursuit" path tracking movement command
void WaypointNavigation::getMovementCommand (base::commands::Motion2D& mc)
{
    //check if std deviation is bigger than the specified std deviation for the current target pose
    for (int i = 0; i<3; i++) {
        //not no sqrt, as both values are sqared, and > is vallid in this case
        if(curPose.cov_position(i,i) > lookaheadPoint.tol_position) {
            std::cout << "Variance of " << i << " is to high " << curPose.cov_position(i,i)
            << " should be smaller than " << lookaheadPoint.tol_position << std::endl;
            mc.translation = 0;
            mc.rotation = 0;
            return;
        }
    }

    if(!targetSet || !poseSet) {
        std::cout << "No target or pose specified" << std::endl;
        mc.translation = 0;
        mc.rotation = 0;
        return;
    }

    // Vector to target position
    Eigen::Vector3d driveVector = lookaheadPoint.position - curPose.position;
    driveVector.z() = 0; // Do not care about robot's Z displacement, navigation in 2D plane
    double distToTarget = driveVector.norm();

    // Transform Lookahead point to Robot Coordinate Frame
    Eigen::Vector3d lookaheadPointRCF(0,0,0);
    lookaheadPointRCF = Eigen::AngleAxisd(-curPose.getYaw(), Eigen::Vector3d::UnitZ()) * (lookaheadPoint.position - curPose.position);

    /*
    std::cout << "Robot, WCF:    \t ("  << curPose.position.x()
                                << ", " << curPose.position.y() << ") " << std::endl;
    std::cout << "Lookahead, WCF:\t ("  << lookaheadPoint.position.x()
                                << ", " << lookaheadPoint.position.y() << ") " << std::endl;
    std::cout << "Lookahead, RCF:\t ("  << lookaheadPointRCF.x()
                                << ", " << lookaheadPointRCF.y() << ") " << std::endl;
	*/

    int sign = ( lookaheadPointRCF.x() < 0 ? -1 : 1);
    if( fabs(lookaheadPointRCF.y()) <=  0.001) {                // Straight line motion if Y below [1mm]
        //std::cout << "Straight line case" << std::endl;
        mc.translation = sign * translationalVelocity;
        mc.rotation = 0;
    } else {                                      // Turn Required
        double turn_radius = (lookaheadPointRCF.x()*lookaheadPointRCF.x() + lookaheadPointRCF.y()*lookaheadPointRCF.y())
        /(2*lookaheadPointRCF.y());

        double theta = atan2(lookaheadPointRCF.y(),lookaheadPointRCF.x())*2; // In Robot Coordinate Frame
        if (theta > M_PI){
            theta -= 2*M_PI;
        } else if (theta < -M_PI) {
            theta += 2*M_PI;
        }
        targetHeading = theta/2;

        // Select Ackermann or Point turn + Straigt line
        // Based on which combination is closer to the target orientation
        // Minimizing the misalignment at target waypoint (presumably points towards the next waypoint)
        double err_straightLine = - (lookaheadPoint.heading - curPose.getYaw());// Target heading in Robot Coordinate frame
        double err_ackermann    = theta +  err_straightLine;               //  By Ackermann turn - Target heading
        err_straightLine       += targetHeading;                          //   By Straight line  - Target heading

        // Maximum distance from the straight line to the waypoint
        // lies in 1/2 the Ackermann turn angular_velocity
        double distFromLine = distToTarget * (1-cos(targetHeading));

        /* Debug
        std::cout << err_straightLine /M_PI*180  << " Error if by straight line (deg)" << std::endl;
        std::cout << err_ackermann /M_PI*180  << " Error if by Ackermann (deg)" << std::endl;
        std::cout << distFromLine <<  " Distance from straight line (m)" << std::endl;
        */
        targetHeading += curPose.getYaw();
        /*
        std::cout   << "R turn\t =\t "
                    << turn_radius << "m" << std::endl;
        std::cout   << "Targ. heading\t =\t "
                    <<  targetHeading/M_PI*180  << " deg" << std::endl;
		*/
        // SELECT THE MORE APPROPRIATE MOTION
        if( fabs(turn_radius) <= minTurnRadius            ||
        //  fabs(err_straightLine) < fabs(err_ackermann)  || // Ackermann more preferred
            fabs(distFromLine)>= maxDisplacementAckermannTurn )
        {
            //std::cout << "PT:\t";
            mc.translation = 0;
            mc.rotation = targetHeading > 0 ? rotationalVelocity : -rotationalVelocity;
        }
        else
        {                                                 // ACKERMANN TURN CASE
            //std::cout << "ACK:\t";
            // targetRotation    = theta;
            // targetTranslation = turn_radius*targetRotation;
            mc.translation = sign * translationalVelocity;
            mc.rotation    = mc.translation / turn_radius;
        }
    }
    /*
    std::cout << "tv = " << mc.translation             << " m/s, ";
    std::cout << "rv = " << mc.rotation  / M_PI *180.0 << " deg/s" << std::endl;
    */
}

/*
Sets the new trajectory and calculates the distances between waypoints
*/
void WaypointNavigation::setTrajectory(std::vector< base::Waypoint *>& t )
{
    // Delete old trajectory
     targetSet = false;
    for(std::vector<base::Waypoint *>::iterator it = trajectory.begin(); it != trajectory.end(); it++)
    {
        delete *it;
    }
    trajectory.clear();
    trajectory = t;
    targetSet 	   = false;
    finalPhase     = false;
    
    

    if(!trajectory.empty()) {
    	// Add current pose at the begining
	    if (poseSet){
	    	initilalizeCurrentSegment();
	    } else {
	    	currentSegment = 0;
	    }
        distanceToNext->resize(trajectory.size()-1);
        base::Vector3d wp;
        for (size_t i = 0; i < distanceToNext->size(); i++) {
            wp  = trajectory.at(i+1)->position; // w2 - w1
            wp -= trajectory.at(i)->position;
            wp.z() = 0;
            distanceToNext->at(i) = wp.norm();
        }
        setNavigationState(DRIVING);
    } else {
        distanceToNext = new std::vector<double>();
        setNavigationState(NO_TRAJECTORY);
    }
    
}

bool  WaypointNavigation::update(base::commands::Motion2D& mc){
    // 1) Update the current SEGMENT
    // Select the segment such that robot is not within immediate reach of the 2nd Waypoint
    double distToNext = (w2-xr).norm();
    while ( distToNext <= corridor ){
        if( currentSegment < trajectory.size()-1){
            setSegmentWaypoint(w1, currentSegment);
            setSegmentWaypoint(w2, currentSegment+1);
            currentSegment++;
            distToNext = (w2-xr).norm();
        } else {
            // LAST SEGMENT HANDLING, vicinity of final waypoint
            // Executing this code means the robot is within the corridor circle of final waypoint
            double headingErr, posErr;
            distToNext = (w2-xr).norm(); // Distance to Final
            // Final heading - current heading
            headingErr  = (trajectory.back()->heading) - curPose.getYaw();
            // TODO Check limit!!!

            // Driving/aligning based on reaching the tolerance of the final radius
            if( distToNext <= trajectory.back()->tol_position ){
                // Position for alignment was reached
                finalPhase = true;
                if( fabs(headingErr) < trajectory.back()->tol_heading){
                    setNavigationState(TARGET_REACHED);
                } else {
                    setNavigationState(ALIGNING);
                    targetHeading = trajectory.back()->heading;
                }
            } else {
                finalPhase = false;
                Eigen::Vector3d xf;
                xf = trajectory.back()->position;
                targetHeading = atan2( xf.y()-xr.y(), xf.x()-xr.x());
                posErr = distToNext * sin(targetHeading-curPose.getYaw());
                if ( fabs(posErr) > trajectory.at(currentSegment)->tol_position ){
                    // This assumes straight line motion, not Ackermann, but approx valid
                    setNavigationState(ALIGNING);
                } else {
                    setNavigationState(DRIVING);
                }
            }
            std::cout << "Final phase:" << std::endl <<
            "\t Dist remng.   \t" << distToNext << " m" << std::endl <<
            "\t Heading error \t" << headingErr*180/M_PI<<
            "/" << trajectory.at(currentSegment)->tol_heading*180/M_PI << " deg" << std::endl <<
            "\t Pos. err. est.\t" << posErr <<" m " <<  std::endl;
            break;
        }
    }

    // 2) Get intersection point with the Path (should also return distance from the segment)
    base::Vector2d xi = getClosestPointOnPath();
    /*
    std::cout << "Closest point: (" 
    	<< xi.x() << ", " << xi.y() << ") "		<< std::endl;
    std::cout << "Dist to next: " << distToNext << std::endl;
    */
    // 3) Calculate the distance from the nominal trajectory
    distanceToPath = (xr-xi).norm();

    NavigationState currentState = getNavigationState();
    /*
    std::cout << "Current segment:\t"   << currentSegment   << std::endl;
    std::cout << "Nav. state:\t\t"      << currentState     << std::endl;
    std::cout << "Dist. from nominal:\t" << distanceToPath  << std::endl;
	*/

    /* -------------------------------------------
    * STATEMACHINE FOR EXECUTION OF TRACKING MODES
    ------------------------------------------- */
    switch (currentState) {
        case (NavigationState)DRIVING:
        {
            // 0) OUT OF BOUNDARIES CHECK
            if ( distanceToPath >= corridor ){
                setNavigationState(OUT_OF_BOUNDARIES);
                mc.translation = 0;
                mc.rotation =0;
                return false;
            }
            double distance;
            distance = distanceToPath + distToNext;

            // i) Get the look ahead point segment
            base::Vector2d lineVector, lookaheadPoint2D;
            std::cout << "Distance: "<< distance << "/" << lookaheadDistance << std::endl;
            if (distance > lookaheadDistance) // Lookahead within same seg.
            {
                // ii) Get the look ahead point
                lineVector = w2-w1;
                lineVector.normalize();
                lookaheadPoint2D = xi + lineVector*(lookaheadDistance-distanceToPath);
            }
            else
            { // Find the right segment
                size_t lookaheadSegment;
                lookaheadSegment = currentSegment;
                for ( ;	lookaheadSegment < distanceToNext->size() &&
                		distance <= lookaheadDistance;
                		lookaheadSegment++)
                {
                    distance +=  distanceToNext->at(lookaheadSegment);
                }

                // ... will extrapolate the segment instead // TODO remove
                if (distance <= lookaheadDistance){
                    // End of trajectory was reached          // setNavigationState(ALIGNING);
                    // Will not execute the ALIGNING case within this update?
                    //         return false;
                    //std::cout << "W.N.: Extrapolating the last segment." << std::endl;
                }
                // ii) Get the look ahead point
                setSegmentWaypoint(l1, lookaheadSegment-1);
                setSegmentWaypoint(l2, lookaheadSegment);
                lineVector = l2-l1;
                lineVector.normalize();
                lookaheadPoint2D = l2 - lineVector*(distance-lookaheadDistance);
            }
            // Set lookahead point: Vector2d -> Waypoint
            lookaheadPoint.position << lookaheadPoint2D(0),lookaheadPoint2D(1),0;
            lookaheadPoint.heading  = atan2(lineVector(1),lineVector(0));
            lookaheadPoint.tol_position = 0.1;
            targetSet = true;
            // iii) Get motion command to the lookahead point
            getMovementCommand(mc);
            if ( fabs(mc.translation) < 1e-6){
                setNavigationState(ALIGNING);
                // Use the Angle To Target as Target heading
            }   // Else drive using the ackermann command values
            break;
        } // --- end of DRIVING ---
        case ALIGNING:
        {
            mc.translation = 0; // Ensure
            double headingErr, disalignmentTolerance;
            disalignmentTolerance = finalPhase ? trajectory.back()->tol_heading : maxDisalignment;
            headingErr  = targetHeading - curPose.getYaw();

            if ( headingErr > disalignmentTolerance*2){
                mc.rotation     =  rotationalVelocity;
            } else if ( headingErr < - disalignmentTolerance*2){
                 mc.rotation    = -rotationalVelocity;
            } else if ( fabs(headingErr) < disalignmentTolerance){
                mc.rotation = 0;
                setNavigationState(DRIVING);
            } else {
                std::cout << "Proportional" << std::endl;
                mc.rotation =  rotationalVelocity/(disalignmentTolerance*2) * headingErr;
            }

            std::cout   << "Aligning:\t " << 180.0/M_PI*curPose.getYaw()<<
                " to " <<          180.0/M_PI*targetHeading  <<
                "+-"   << 180.0/M_PI*disalignmentTolerance << " deg" <<
                ",\t rv = "<<          180.0/M_PI*mc.rotation    <<
                "deg/s."      << std::endl;

            break;
        } // --- end of ALIGNING ---
        case OUT_OF_BOUNDARIES:
        {
            mc.translation = 0;
            mc.rotation    = 0;
            double progress, distAlong, distPerpendicular;
            // Try the current segment first
            if(currentSegment > 0){
                getProgressOnSegment(currentSegment, progress, distAlong, distPerpendicular);
                if( progress > 0  && isInsideBoundaries(distAlong, distPerpendicular) )
                {
                    /*
                    std::cout << "Resolved, robot was in the current segment with progres of: "
                    << progress*100 << "\%" << std::endl;
                    */
                   setNavigationState(DRIVING);
                   break;
               	} else {
               		/*
                   std::cout << "Robot not found in the current segment corridor" << std::endl;
                   std::cout << "\t Progress (%): \t"<< progress*100 << std::endl;
                   std::cout << "\t Longitudal err (m): \t"<< distAlong << std::endl;
                   std::cout << "\t Lateral error (m): \t" << distPerpendicular << std::endl;
                   */
               	}
            }
            // Try the previous or next segment
            if(currentSegment > 1){
               getProgressOnSegment(currentSegment-1, progress, distAlong, distPerpendicular);
               	if ( isInsideBoundaries(distAlong, distPerpendicular) )
                {
                    currentSegment--;
                    setSegmentWaypoint(w1, currentSegment-1);
                    setSegmentWaypoint(w2, currentSegment);
                    setNavigationState(DRIVING);
                    // Robot is in the "Bounding box: of the previous segment
                    /*std::cout << "Resolved, robot was in previous segment with progres of: "
                    << progress*100 << "\%, segment decremented" << std::endl;
                    std:: cout << w1 << std::endl << w2 << std::endl;
                    */
                    break;
                } else {
                	/*
                    std::cout << "Robot not found in the previous segments corridor" << std::endl;
                    std::cout << "\t Progress (%): \t"<< progress*100 << std::endl;
                    std::cout << "\t Longitudal err (m): \t"<< distAlong << std::endl;
                    std::cout << "\t Lateral error (m): \t" << distPerpendicular << std::endl;
                	*/
                }
            }
            // try matching the robot position with the NEXT segment
            if(currentSegment < trajectory.size()-1)
            {
                getProgressOnSegment(currentSegment+1, progress, distAlong, distPerpendicular);
                if ( isInsideBoundaries(distAlong, distPerpendicular) )
                {
                    currentSegment++;
                    setSegmentWaypoint(w1, currentSegment-1);
                    setSegmentWaypoint(w2, currentSegment);
                    std:: cout << w1 << std::endl << w2 << std::endl;
                    setNavigationState(DRIVING);
                    /* Robot is in the "Bounding box: of the next segment /
                    std::cout << "Resolved, robot was in next segment with progres of: "
                    << progress*100 << "\%, segment incremented" << std::endl;
                    */
                    break;
                } else {
                    /*
                    std::cout << "Robot not found in the next segments corridor" << std::endl;
                    std::cout << "\t Progress (%): \t"<< progress*100 << std::endl;
                    std::cout << "\t Longitudal err (m): \t"<< distAlong << std::endl;
                    std::cout << "\t Lateral error (m): \t" << distPerpendicular << std::endl;
	  				*/                  
                }
            }
            break;
        }
        case TARGET_REACHED:
            std::cout << "Target Reached." 		<< std::endl;
            break;
        case NO_TRAJECTORY:
            std::cout << "Invalid trajectory." 	<< std::endl;
            break;
        default:
            std::cout<<"Default case."<<std::endl;
            break;
    }
    return true;
}

base::Vector2d WaypointNavigation::getClosestPointOnPath(){
    // Solving for parameter k such that closestPoint = w1 + k*segVector;
    // 1) segVector = (w2-w1) Vector of the segment line
    // Using the equations
    //    a) w1 + segVector*k = xi
    //    b) xr + perpendicular(segVector)*j = xi
    // This gives [xr - w1] = [perpendicular(segVector), segVector][j; k]
    // 2) Calculate k using the inverse matrix
    // 3) Calculate the point of intersection using a)

    // 1)
    base::Vector2d segVector = w2 - w1;
    segVector.normalize();
    // 2)
    base::Matrix2d inverseL;
    inverseL  << segVector(1), -segVector(0),
    segVector(0),  segVector(1);
    base::Vector2d xi = inverseL*(xr-w1);
    // 3)
    xi = w1 + xi(1)*segVector;
    return xi;
}

bool WaypointNavigation::setSegmentWaypoint(base::Vector2d& waypoint, int indexSegment){
    if (indexSegment > ((int)trajectory.size()-1) || indexSegment<0)
    return false;
    waypoint << trajectory.at(indexSegment)->position(0),
    trajectory.at(indexSegment)->position(1);
    return true;
}

const base::Waypoint* WaypointNavigation::getLookaheadPoint(){
    return &lookaheadPoint;
}

//return true;
/*
Eigen::Quaterniond oriDiffToTarget = curPose.orientation.inverse() * Eigen::Quaterniond(Eigen::AngleAxisd(targetPose.heading, Eigen::Vector3d::UnitZ()));
Eigen::Vector3d t = oriDiffToTarget * Eigen::Vector3d(1.0,0,0);
double anglediff = atan2(t.y(), t.x());

if(fabs(anglediff) < maxDisalignment)
return true;

return false;
*/

bool WaypointNavigation::configure(double minR,	double tv, double rv,
    double cr, double lad)
    {
        std::cout << "Received Path Tracker config values:" << std::endl <<
        "Rmin:\t" << minR << std::endl     <<
        "tv:\t" << tv << std::endl       <<
        "rv:\t" << rv << std::endl       <<
        "cr:\t" << cr << std::endl       <<
        "lh:\t" << lad << std::endl;
        // All config. parameters must be possitive
        if( minR>0 && tv>0 && rv>0 && cr > 0 && lad > 0){
            minTurnRadius 		= minR;
            translationalVelocity 	= tv;
            rotationalVelocity 	= rv;
            corridor		= cr;
            lookaheadDistance	= lad;
            // std::cout << "Config successful, " << getLookaheadDistance() << std::endl;
            return true;
        } else {
            return false;
        }
    }

    bool WaypointNavigation::getProgressOnSegment(int segmentNumber,
        double& progress, double& distAlong, double& distPerpend){
            // Solving for parameter k such that closestPoint = w1 + k*segVector;
            // 1) segVector = (w2-w1) Vector of the segment line
            // Using the equations
            //    a) w1 + segVector*k = xi
            //    b) xr + perpendicular(segVector)*j = xi
            // This gives [xr - w1] = [perpendicular(segVector), segVector][j; k]
            // 2) Calculate k using the inverse matrix
            // 3) Calculate the point of intersection using a)
            // k is also the progress along that segment

            // Using wStart and wEnd instead of w1, w2
            // 1)
            base::Vector2d segVector, wStart, wEnd;
            setSegmentWaypoint(wStart, segmentNumber-1);
            setSegmentWaypoint(wEnd  , segmentNumber);
            segVector = wEnd-wStart;

            // 2)
            double determinant = segVector.dot(segVector);
            base::Matrix2d inverseL;
            inverseL  << segVector(1), -segVector(0),
                         segVector(0),  segVector(1);
            inverseL /= determinant;

            base::Vector2d xi = inverseL*(xr-wStart);

            // 3)
            progress =  xi(1);
            xi = wStart + progress*segVector;
            distPerpend = (xr-xi).norm();

            if ( progress >= 1 ) {
                distAlong = (progress-1)*segVector.norm();
            } else if (progress <= 0 ) {
                distAlong = progress*segVector.norm();
            } else{
                distAlong = 0;
            }
            /* // DEBUG OUTPUTS
            std::cout << "seg vector = (" << segVector(0) <<
            ", " << segVector(1) << ")" << std::endl;
            std::cout << "xr = (" << xr(0) <<
            ", " << xr(1) << ")" << std::endl;
             std::cout << "wStart = (" << wStart(0) <<
            ", " << wStart(1) << ")" << std::endl;
            std::cout << "Determinant: " << determinant << std::endl;
            std::cout << "solution = (" << xi(0) <<
            ", " << xi(1)? << ")" << std::endl;
            std::cout << inverseL << std::endl;
            */
            return true;
        }

        void WaypointNavigation::setCurrentSegment(int segmentNumber){
        	if (segmentNumber < 0){
        		std::cerr << "Attemp to set invalid segment number" << std::endl;
        		return;
        	}
        	if ( segmentNumber < 1){
        		w1 << curPose.position(0), curPose.position(1);
	        	setSegmentWaypoint(w2, segmentNumber);
        	} else {
        		setSegmentWaypoint(w2, segmentNumber-1);
        		setSegmentWaypoint(w2, segmentNumber);
        	}
        	currentSegment = segmentNumber;
        }
    
        void WaypointNavigation::initilalizeCurrentSegment(){
		/* For each segment (except the zeroth (trivial) one): 
		*	a) calculate the hypothetical progress on the segment
		*  	b) store the maximum index where the robot was in corridor bounds 
		*  Set the segment to the max index found.	
		*/
        	size_t maxIndex = 0;
        	double progress, distAlong, distPerpend;
        	for (size_t i = 1; i < trajectory.size(); i++) {
        		getProgressOnSegment(i, progress, distAlong, distPerpend);
        		if ( progress >= 0 && progress <= 1
            		 && distPerpend < corridor){
            		maxIndex = i;
            	}
            	std::cout << "Segment: " << i << ", progress: " << progress << 
        			", dist from nominal " << distPerpend << std::endl;
        	}
        	std::cout << "Segment set to: " << maxIndex << std::endl;
        	setCurrentSegment(maxIndex);
        }

        bool WaypointNavigation::isInsideBoundaries(double& distAlong, double& distPerpend){
        	return  fabs(distAlong)		< corridor &&
                    fabs(distPerpend)	< corridor;
        }
}
